/* atlifm plugin api providing c abi version 1 for native plugins
 *
 * plugins are shared objects inside user config plugins directory exporting atlifm_plugin_init
 * the host loads them via dlopen and supplies the atlifmhostapi structure
 * registrations occur strictly during initialization after which the table is sealed
 *
 * threading model:
 * run and override and hook execute in the gui thread
 * preview render runs in a worker thread pool where only write and custom logic are permitted
 * identifiers with sudo are protected and bypass plugin dispatch entirely */
#ifndef ATLIFM_PLUGIN_API_H
#define ATLIFM_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ATLIFM_PLUGIN_ABI_VERSION 1u

typedef struct AtlifmHostApi AtlifmHostApi;

/* invocation context containing event name and action id and cwd and selected paths
 * all strings are utf8 and remain valid only during this call */
typedef struct AtlifmInvocation {
    uint32_t struct_size;
    const char* event;
    const char* action;
    const char* cwd;
    const char* const* selection;
    size_t selection_count;
    const char* query;
} AtlifmInvocation;

/* text sink where preview implementations stream chunks of output data */
typedef void (*AtlifmWriteFn)(void* write_user, const char* data, size_t len);

/* action descriptor defining custom menu entries and key bindings
 * id must start with a letter and contain only alphanumeric characters or underscores
 * applies_to accepts any or files or dirs while extensions lists matching file extensions
 * enabled is optional where non positive value hides the action and run returns 0 on success */
typedef struct AtlifmActionDesc {
    uint32_t struct_size;
    const char* id;
    const char* name;
    const char* description;
    const char* keys;
    const char* applies_to;
    const char* extensions;
    void* user;
    int (*run)(const AtlifmInvocation* inv, void* user);
    int (*enabled)(const AtlifmInvocation* inv, void* user);
    const char* command;
    const char* command_mode;
    const char* command_title;
    const char* command_description;
    const char* command_placeholder;
} AtlifmActionDesc;

/* override handler returning 1 if event was handled or 0 to trigger fallback
 * intercepts actions triggered by keys or menus or mouse except privileged sudo commands */
typedef int (*AtlifmOverrideFn)(const AtlifmInvocation* inv, void* user);

/* preview renderer receiving target file path and write callback for text
 * empty output signals fallback to builtin preview and extensions are mandatory */
typedef void (*AtlifmPreviewFn)(const char* path, AtlifmWriteFn write,
                                void* write_user, void* user);

/* lifecycle hooks triggered on start and navigate and select events */
typedef void (*AtlifmHookFn)(const AtlifmInvocation* inv, void* user);

struct AtlifmHostApi {
    uint32_t abi_version;
    uint32_t struct_size;

    const char* (*host_version)(void);
    /* writes text to action output overlay, or to the center pane of a ctrl+f command output */
    void (*output_write)(const char* text);
    /* displays short message inside status bar */
    void (*notify)(const char* message);
    /* launches external command where detach 1 runs background process and detach 0 streams output to the same sink as output_write */
    int (*run_command)(const char* command_line, int detach);
    /* refreshes file listings and bookmarks after disk modifications */
    void (*refresh)(void);

    int (*register_action)(const AtlifmActionDesc* desc);
    int (*register_override)(const char* action_id, AtlifmOverrideFn run, void* user);
    int (*register_preview)(const char* extensions, AtlifmPreviewFn render, void* user);
    int (*register_hook)(const char* event, AtlifmHookFn fn, void* user);
    void (*log)(const char* level, const char* message);
    /* writes text to one pane of the three-pane command output: 0 parent, 1 center, 2 preview
     * the first call opens the pane view; utf-8 text is appended as is */
    void (*output_write_pane)(int pane, const char* text);
};

/* initialization entrypoint returning 0 on success or failing if abi_version mismatches
 * shutdown is optional */
int atlifm_plugin_init(const AtlifmHostApi* api);
void atlifm_plugin_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
