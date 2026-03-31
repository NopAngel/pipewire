/* PipeWire Control Implementation */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <spa/pod/parser.h>
#include <spa/debug/types.h>

#include <pipewire/control.h>
#include <pipewire/private.h>

#define NAME "control"

struct impl {
    struct pw_control this;
    struct pw_memblock *mem;
};

/**
 * Create a new PipeWire control.
 */
struct pw_control *
pw_control_new(struct pw_context *context,
           struct pw_impl_port *port,
           uint32_t id, uint32_t size,
           size_t user_data_size)
{
    struct impl *impl;
    struct pw_control *this;
    enum spa_direction direction;

    /* Determine direction based on IO type */
    switch (id) {
    case SPA_IO_Control:
        direction = SPA_DIRECTION_INPUT;
        break;
    case SPA_IO_Notify:
        direction = SPA_DIRECTION_OUTPUT;
        break;
    default:
        errno = ENOTSUP;
        return NULL;
    }

    /* Allocate implementation and user data in one block */
    impl = calloc(1, sizeof(struct impl) + user_data_size);
    if (impl == NULL)
        return NULL;

    this = &impl->this;
    this->id = id;
    this->size = size;
    this->context = context;
    this->port = port;
    this->direction = direction;

    pw_log_debug(NAME" %p: new %s %d", this,
             spa_debug_type_find_name(spa_type_io, this->id), direction);

    spa_list_init(&this->links);
    spa_hook_list_init(&this->listener_list);

    if (user_data_size > 0)
        this->user_data = SPA_PTROFF(impl, sizeof(struct impl), void);

    /* Register control in context and port lists */
    spa_list_append(&context->control_list[direction], &this->link);
    if (port) {
        spa_list_append(&port->control_list[direction], &this->port_link);
        pw_impl_port_emit_control_added(port, this);
    }
    return this;
}

/**
 * Destroy the control and release resources.
 */
void pw_control_destroy(struct pw_control *control)
{
    struct impl *impl = SPA_CONTAINER_OF(control, struct impl, this);
    struct pw_control_link *link;

    pw_log_debug(NAME" %p: destroy", control);
    pw_control_emit_destroy(control);

    /* Remove all associated links */
    bool is_output = (control->direction == SPA_DIRECTION_OUTPUT);
    spa_list_consume(link, &control->links, is_output ? out_link : in_link)
        pw_control_remove_link(link);

    spa_list_remove(&control->link);

    if (control->port) {
        spa_list_remove(&control->port_link);
        pw_impl_port_emit_control_removed(control->port, control);
    }

    pw_log_debug(NAME" %p: free", control);
    pw_control_emit_free(control);
    spa_hook_list_clean(&control->listener_list);

    /* Cleanup shared memory if this was an output control */
    if (is_output && impl->mem)
        pw_memblock_unref(impl->mem);

    free(impl);
}

SPA_EXPORT
struct pw_impl_port *pw_control_get_port(struct pw_control *control)
{
    return control->port;
}

SPA_EXPORT
void pw_control_add_listener(struct pw_control *control,
                 struct spa_hook *listener,
                 const struct pw_control_events *events,
                 void *data)
{
    spa_hook_list_append(&control->listener_list, listener, events, data);
}

/**
 * Internal helper to set IO on port or node.
 */
static int port_set_io(struct pw_impl_port *port, uint32_t mix, uint32_t id, void *data, uint32_t size)
{
    int res;

    /* Try setting IO on the mixer first, then fallback to the node */
    if (port->mix) {
        res = spa_node_port_set_io(port->mix, port->direction, mix, id, data, size);
        if (SPA_RESULT_IS_OK(res))
            return res;
    }

    res = spa_node_port_set_io(port->node->node, port->direction, port->port_id, id, data, size);
    if (res < 0) {
        pw_log_warn("port %p: set io failed %d (%s)", port, res, spa_strerror(res));
    }
    return res;
}

SPA_EXPORT
int pw_control_add_link(struct pw_control *control, uint32_t cmix,
        struct pw_control *other, uint32_t omix,
        struct pw_control_link *link)
{
    struct impl *impl;
    uint32_t size;
    int res = 0;

    /* Ensure control is output and other is input */
    if (control->direction == SPA_DIRECTION_INPUT) {
        SPA_SWAP(control, other);
        SPA_SWAP(cmix, omix);
    }
    
    if (control->direction != SPA_DIRECTION_OUTPUT || other->direction != SPA_DIRECTION_INPUT)
        return -EINVAL;

    impl = SPA_CONTAINER_OF(control, struct impl, this);
    size = SPA_MAX(control->size, other->size);

    /* Allocate shared memory block if not present */
    if (impl->mem == NULL) {
        impl->mem = pw_mempool_alloc(control->context->pool,
                        PW_MEMBLOCK_FLAG_READWRITE |
                        PW_MEMBLOCK_FLAG_SEAL |
                        PW_MEMBLOCK_FLAG_MAP,
                        SPA_DATA_MemFd, size);
        if (impl->mem == NULL)
            return -errno;
    }

    /* Set IO for output control if it's the first link */
    if (spa_list_is_empty(&control->links) && control->port) {
        if ((res = port_set_io(control->port, cmix, control->id, impl->mem->map->ptr, size)) < 0)
            return res;
    }

    /* Set IO for input control */
    if (other->port) {
        if ((res = port_set_io(other->port, omix, other->id, impl->mem->map->ptr, size)) < 0)
            return res;
    }

    /* Initialize link structure */
    link->output = control;
    link->input = other;
    link->out_port = cmix;
    link->in_port = omix;
    link->valid = true;
    
    spa_list_append(&control->links, &link->out_link);
    spa_list_append(&other->links, &link->in_link);

    pw_control_emit_linked(control, other);
    pw_control_emit_linked(other, control);

    return res;
}

SPA_EXPORT
int pw_control_remove_link(struct pw_control_link *link)
{
    struct pw_control *output = link->output;
    struct pw_control *input = link->input;
    int res = 0;

    pw_log_debug(NAME" %p: unlink from %p", output, input);

    spa_list_remove(&link->in_link);
    spa_list_remove(&link->out_link);
    link->valid = false;

    /* Unset IO if no links remain on the output */
    if (spa_list_is_empty(&output->links)) {
        res = port_set_io(output->port, link->out_port, output->id, NULL, 0);
    }

    /* Always unset IO on the input port being removed */
    if (input->port) {
        int r = port_set_io(input->port, link->in_port, input->id, NULL, 0);
        if (res == 0) res = r;
    }

    pw_control_emit_unlinked(output, input);
    pw_control_emit_unlinked(input, output);

    return res;
}