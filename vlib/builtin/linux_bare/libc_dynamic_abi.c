#include "libc_compat.h"
#include "libc_dynamic_abi.h"
#include <dlfcn.h>
#include <link.h>
#include <string.h>
/* Public loader metadata, native callback ABI and per-thread storage only. */
/* DT_DEBUG is filled in by the interpreter before the executable's preinit
 * callbacks. Looking up _r_debug as an imported symbol would require an
 * explicit dependency on ld.so, which older glibc treats as a libc process. */
static struct r_debug *v_dynamic_debug(void) {
    extern ElfW(Dyn) _DYNAMIC[] __attribute__((weak));
    if (_DYNAMIC) {
        for (const ElfW(Dyn) *d = _DYNAMIC; d->d_tag != DT_NULL; ++d)
            if (d->d_tag == DT_DEBUG) return (struct r_debug *)d->d_un.d_ptr;
    }
    return NULL;
}
uintptr_t v_dynamic_head(void) {
    struct r_debug *debug = v_dynamic_debug();
    return debug ? (uintptr_t)debug->r_map : 0;
}
/* The interpreter need not appear in r_map when no object names it in
 * DT_NEEDED. Its ELF image and dynamic section are still mapped. */
extern uintptr_t v_dynamic_map_symbol(uintptr_t, const char *);
uintptr_t v_bare_loader_symbol(const char *name) {
    struct r_debug *debug = v_dynamic_debug();
    if (!debug || !debug->r_ldbase) return 0;
    ElfW(Addr) base = debug->r_ldbase;
    const ElfW(Ehdr) *header = (const ElfW(Ehdr) *)base;
    const ElfW(Phdr) *ph = (const ElfW(Phdr) *)(base + header->e_phoff);
    for (size_t i = 0; i < header->e_phnum; ++i) {
        if (ph[i].p_type == PT_DYNAMIC) {
            struct link_map interpreter = {
                .l_addr = base, .l_ld = (ElfW(Dyn) *)(base + ph[i].p_vaddr)
            };
            return v_dynamic_map_symbol((uintptr_t)&interpreter, name);
        }
    }
    return 0;
}

struct v_fini_object {
    struct link_map *map;
    const char *strings, *soname;
    unsigned char seen;
};

static void v_fini_visit(struct v_fini_object *objects, size_t count, size_t index,
                         struct link_map **order, size_t *used) {
    struct v_fini_object *object = &objects[index];
    if (object->seen) return;
    object->seen = 1;
    for (const ElfW(Dyn) *d = object->map->l_ld; d->d_tag != DT_NULL; ++d) {
        if (d->d_tag != DT_NEEDED) continue;
        const char *needed = object->strings + d->d_un.d_val;
        for (size_t j = 0; j < count; ++j) {
            const char *path = objects[j].map->l_name;
            const char *slash = strrchr(path, '/');
            if ((objects[j].soname && !strcmp(needed, objects[j].soname)) ||
                !strcmp(needed, path) || (slash && !strcmp(needed, slash + 1))) {
                v_fini_visit(objects, count, j, order, used);
                break;
            }
        }
    }
    order[(*used)++] = object->map;
}

/* glibc 2.39 counts the interpreter even when it removes it from r_map.
 * Its _dl_fini then aborts before calling any destructor. In this case
 * finalize startup objects in reverse dependency order using the ELF ABI. */
int v_dynamic_finalize_without_interpreter(void) {
    struct r_debug *debug = v_dynamic_debug();
    if (!debug || !debug->r_map) return 0;
    size_t count = 0;
    for (struct link_map *m = debug->r_map; m; m = m->l_next) {
        if (m->l_addr == debug->r_ldbase) return 0;
        ++count;
    }
    struct v_fini_object objects[count];
    struct link_map *order[count];
    size_t used = 0, index = 0;
    for (struct link_map *m = debug->r_map; m; m = m->l_next) {
        struct v_fini_object *o = &objects[index++];
        *o = (struct v_fini_object){ .map = m };
        size_t soname = 0;
        for (const ElfW(Dyn) *d = m->l_ld; d->d_tag != DT_NULL; ++d) {
            if (d->d_tag == DT_STRTAB) {
                uintptr_t addr = d->d_un.d_ptr;
                o->strings = (const char *)(addr < m->l_addr ? m->l_addr + addr : addr);
            }
            if (d->d_tag == DT_SONAME) soname = d->d_un.d_val;
        }
        if (soname) o->soname = o->strings + soname;
    }
    for (size_t i = count; i; --i) v_fini_visit(objects, count, i - 1, order, &used);
    while (used) {
        struct link_map *m = order[--used];
        void (**array)(void) = NULL;
        void (*fini)(void) = NULL;
        size_t length = 0;
        for (const ElfW(Dyn) *d = m->l_ld; d->d_tag != DT_NULL; ++d) {
            if (d->d_tag == DT_FINI_ARRAY) array = (void (**)(void))(m->l_addr + d->d_un.d_ptr);
            if (d->d_tag == DT_FINI_ARRAYSZ) length = d->d_un.d_val / sizeof(*array);
            if (d->d_tag == DT_FINI) fini = (void (*)(void))(m->l_addr + d->d_un.d_ptr);
        }
        while (length) array[--length]();
        if (fini) fini();
    }
    return 1;
}
uintptr_t v_dynamic_field(uintptr_t object, int field) {
    const struct link_map *map = (const struct link_map *)object;
    switch (field) {
    case 0: return map->l_addr;
    case 1: return (uintptr_t)map->l_name;
    case 2: return (uintptr_t)map->l_ld;
    case 3: return (uintptr_t)map->l_next;
    default: return (uintptr_t)map->l_prev;
    }
}
static __thread char v_dynamic_error[512];
static __thread int v_dynamic_pending;
uintptr_t v_dynamic_error_buffer(void) { return (uintptr_t)v_dynamic_error; }
int v_dynamic_error_pending(int value) {
    int previous = v_dynamic_pending;
    if (value >= 0) v_dynamic_pending = value;
    return previous;
}
uintptr_t v_dynamic_ifunc(uintptr_t function) {
    #if defined(__aarch64__)
    extern uintptr_t v_native_getauxval(uintptr_t);
    return ((uintptr_t (*)(uintptr_t))function)(v_native_getauxval(16));
#else
    return ((uintptr_t (*)(void))function)();
#endif
}
extern uintptr_t v_dynamic_open(uintptr_t, int32_t);
extern uintptr_t v_dynamic_symbol(uintptr_t, uintptr_t, uintptr_t);
extern int32_t v_dynamic_close(uintptr_t);
extern uintptr_t v_dynamic_error_get(void);
void *dlopen(const char *name, int flags) { return (void *)v_dynamic_open((uintptr_t)name, flags); }
void *dlsym(void *handle, const char *name) {
    return (void *)v_dynamic_symbol((uintptr_t)handle, (uintptr_t)name,
                                  (uintptr_t)__builtin_return_address(0));
}
int dlclose(void *handle) { return v_dynamic_close((uintptr_t)handle); }
char *dlerror(void) { return (char *)v_dynamic_error_get(); }
extern int32_t v_dynamic_address(uintptr_t, uintptr_t);
extern int32_t v_dynamic_address_extra(uintptr_t, uintptr_t, uintptr_t, int32_t);
extern int32_t v_dynamic_info(uintptr_t, int32_t, uintptr_t);
extern int32_t v_dynamic_iterate(uintptr_t, uintptr_t);
int dladdr(const void *address, Dl_info *info) { return v_dynamic_address((uintptr_t)address,(uintptr_t)info); }
int dladdr1(const void *address, Dl_info *info, void **extra, int flags) {
    return v_dynamic_address_extra((uintptr_t)address,(uintptr_t)info,(uintptr_t)extra,flags);
}
int dlinfo(void *handle,int request,void *output) { return v_dynamic_info((uintptr_t)handle,request,(uintptr_t)output); }
void v_dynamic_address_set(uintptr_t output, uintptr_t file, uintptr_t base, uintptr_t name, uintptr_t symbol) {
    Dl_info *info=(Dl_info *)output;
    info->dli_fname=(const char *)file; info->dli_fbase=(void *)base;
    info->dli_sname=(const char *)name; info->dli_saddr=(void *)symbol;
}
int v_dynamic_visit(uintptr_t callback, uintptr_t data, uintptr_t base, uintptr_t name, uintptr_t phdr, uint16_t count) {
    struct dl_phdr_info info={0};
    info.dlpi_addr=base; info.dlpi_name=(const char *)name;
    info.dlpi_phdr=(const ElfW(Phdr) *)phdr; info.dlpi_phnum=count;
    /* Only advertise the public prefix whose contents are available here. */
    return ((int (*)(struct dl_phdr_info *,size_t,void *))callback)(
        &info,offsetof(struct dl_phdr_info,dlpi_adds),(void *)data);
}
int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *,size_t,void *),void *data) {
    return v_dynamic_iterate((uintptr_t)callback,(uintptr_t)data);
}
extern int32_t v_dynamic_find_object(uintptr_t,uintptr_t);
int _dl_find_object(void *address,struct dl_find_object *output) {
    return v_dynamic_find_object((uintptr_t)address,(uintptr_t)output);
}
void v_dynamic_find_set(uintptr_t output,uintptr_t start,uintptr_t end,uintptr_t map,uintptr_t eh,uintptr_t sframe) {
    struct dl_find_object *info=(struct dl_find_object *)output;
    *info=(struct dl_find_object){0};
    info->dlfo_map_start=(void *)start; info->dlfo_map_end=(void *)end;
    info->dlfo_link_map=(struct link_map *)map; info->dlfo_eh_frame=(void *)eh;
#ifdef DLFO_FLAG_SFRAME
    info->dlfo_sframe=(void *)sframe;
    if(sframe) info->dlfo_flags=DLFO_FLAG_SFRAME;
#else
    (void)sframe;
#endif
}
