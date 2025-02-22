/*
 * QEMU host private memfd memory backend
 *
 * Copyright (C) 2021 Intel Corporation
 *
 * Authors:
 *   Chao Peng <chao.p.peng@linux.intel.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "sysemu/hostmem.h"
#include "qom/object_interfaces.h"
#include "qemu/memfd.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qom/object.h"

#define TYPE_MEMORY_BACKEND_MEMFD_PRIVATE "memory-backend-memfd-private"

OBJECT_DECLARE_SIMPLE_TYPE(HostMemoryBackendPrivateMemfd,
                           MEMORY_BACKEND_MEMFD_PRIVATE)


struct HostMemoryBackendPrivateMemfd {
    HostMemoryBackend parent_obj;
    HostMemoryBackend *shmem;

    bool hugetlb;
    uint64_t hugetlbsize;
    char *path;
};

static void
priv_memfd_backend_memory_alloc(HostMemoryBackend *backend, Error **errp)
{
    HostMemoryBackendPrivateMemfd *m = MEMORY_BACKEND_MEMFD_PRIVATE(backend);
    uint32_t ram_flags;
    char *name;
    int fd, priv_fd;
    unsigned int flags;
    int mount_fd;

    if (!backend->size) {
        error_setg(errp, "can't create backend with size 0");
        return;
    }

    if (m->shmem) {
        assert(m->shmem->mr);
        backend->mr = m->shmem->mr;
    } else {
        fd = qemu_memfd_create("memory-backend-memfd-shared", backend->size,
                               m->hugetlb, m->hugetlbsize, 0, errp);
        if (fd == -1) {
            return;
        }

        name = host_memory_backend_get_name(backend);
        ram_flags = backend->share ? RAM_SHARED : 0;
        ram_flags |= backend->reserve ? 0 : RAM_NORESERVE;
        memory_region_init_ram_from_fd(backend->mr, OBJECT(backend), name,
                                       backend->size, ram_flags, fd, 0, errp);
        g_free(name);
    }

    flags = 0;
    mount_fd = -1;
    if (m->path) {
        flags = RMFD_USERMNT;
        mount_fd = open_tree(AT_FDCWD, m->path, OPEN_TREE_CLOEXEC);
        if (mount_fd == -1) {
            error_setg(errp, "open_tree() failed at %s: %s",
                       m->path, strerror(errno));
            return;
        }
    }
    priv_fd = qemu_memfd_restricted(backend->size, flags, mount_fd, errp);
    if (mount_fd >= 0) {
        close(mount_fd);
    }
    if (priv_fd == -1) {
        return;
    }

    memory_region_set_restricted_fd(backend->mr, priv_fd);
    ram_block_alloc_cgs_bitmap(backend->mr->ram_block);
}

static bool
priv_memfd_backend_get_hugetlb(Object *o, Error **errp)
{
    return MEMORY_BACKEND_MEMFD_PRIVATE(o)->hugetlb;
}

static void
priv_memfd_backend_set_hugetlb(Object *o, bool value, Error **errp)
{
    MEMORY_BACKEND_MEMFD_PRIVATE(o)->hugetlb = value;
}

static void
priv_memfd_backend_set_hugetlbsize(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    HostMemoryBackendPrivateMemfd *m = MEMORY_BACKEND_MEMFD_PRIVATE(obj);
    uint64_t value;

    if (host_memory_backend_mr_inited(MEMORY_BACKEND(obj))) {
        error_setg(errp, "cannot change property value");
        return;
    }

    if (!visit_type_size(v, name, &value, errp)) {
        return;
    }
    if (!value) {
        error_setg(errp, "Property '%s.%s' doesn't take value '%" PRIu64 "'",
                   object_get_typename(obj), name, value);
        return;
    }
    m->hugetlbsize = value;
}

static void
priv_memfd_backend_get_hugetlbsize(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    HostMemoryBackendPrivateMemfd *m = MEMORY_BACKEND_MEMFD_PRIVATE(obj);
    uint64_t value = m->hugetlbsize;

    visit_type_size(v, name, &value, errp);
}

static char *priv_memfd_backend_get_path(Object *obj, Error **errp)
{
    HostMemoryBackendPrivateMemfd *m = MEMORY_BACKEND_MEMFD_PRIVATE(obj);

    return g_strdup(m->path);
}

static void priv_memfd_backend_set_path(Object *obj, const char *value, Error **errp)
{
    HostMemoryBackendPrivateMemfd *m = MEMORY_BACKEND_MEMFD_PRIVATE(obj);

    g_free(m->path);
    m->path = g_strdup(value);
}

static void
priv_memfd_backend_instance_init(Object *obj)
{
    MEMORY_BACKEND(obj)->reserve = false;
}

static void
priv_memfd_backend_class_init(ObjectClass *oc, void *data)
{
    HostMemoryBackendClass *bc = MEMORY_BACKEND_CLASS(oc);

    bc->alloc = priv_memfd_backend_memory_alloc;

    object_class_property_add_str(oc, "path",
                                  priv_memfd_backend_get_path,
                                  priv_memfd_backend_set_path);
    object_class_property_set_description(oc, "path",
                                          "path to mount point of shmfs");

    object_class_property_add_link(oc,
                                   "shmemdev",
                                   TYPE_MEMORY_BACKEND,
                                   offsetof(HostMemoryBackendPrivateMemfd, shmem),
                                   object_property_allow_set_link,
                                   OBJ_PROP_LINK_STRONG);
    object_class_property_set_description(oc, "shmemdev",
                                          "memory backend for shared memory");

    if (qemu_memfd_check(MFD_HUGETLB)) {
        object_class_property_add_bool(oc, "hugetlb",
                                       priv_memfd_backend_get_hugetlb,
                                       priv_memfd_backend_set_hugetlb);
        object_class_property_set_description(oc, "hugetlb",
                                              "Use huge pages");
        object_class_property_add(oc, "hugetlbsize", "int",
                                  priv_memfd_backend_get_hugetlbsize,
                                  priv_memfd_backend_set_hugetlbsize,
                                  NULL, NULL);
        object_class_property_set_description(oc, "hugetlbsize",
                                              "Huge pages size (ex: 2M, 1G)");
    }
}

static const TypeInfo priv_memfd_backend_info = {
    .name = TYPE_MEMORY_BACKEND_MEMFD_PRIVATE,
    .parent = TYPE_MEMORY_BACKEND,
    .instance_init = priv_memfd_backend_instance_init,
    .class_init = priv_memfd_backend_class_init,
    .instance_size = sizeof(HostMemoryBackendPrivateMemfd),
};

static void register_types(void)
{
    if (qemu_memfd_check(MFD_ALLOW_SEALING)) {
        type_register_static(&priv_memfd_backend_info);
    }
}

type_init(register_types);
