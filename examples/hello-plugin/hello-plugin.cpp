/* atlifm example plugin (C ABI v1)
 *
 * build from the repo root, header lives in atlifm-core/:
 *   c++ -shared -fPIC -Iatlifm-core -O2 -o hello.so examples/hello-plugin/hello-plugin.cpp
 *
 * install:
 *   mkdir -p ~/.config/atlifm/plugins
 *   cp hello.so ~/.config/atlifm/plugins/
 *
 * on startup the plugin registers:
 *   action hello_banner, Plugins menu item, default Ctrl+Alt+H
 *   override for properties, it declines handling and falls back to the builtin behavior
 *   preview for *.hello, first lines of the file as text
 *   hook on_start, writes a line to the log
 *
 * log goes to /tmp/atlifm-hello.log or the path in ATLIFM_PLUGIN_LOG */
#include "atlifm-plugin-api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

const char* logPath() {
    const char* path = std::getenv("ATLIFM_PLUGIN_LOG");
    return (path && *path) ? path : "/tmp/atlifm-hello.log";
}

void pluginLog(const std::string& line) {
    FILE* f = std::fopen(logPath(), "a");
    if (!f) return;
    std::fputs(line.c_str(), f);
    std::fputc('\n', f);
    std::fclose(f);
}

std::string quote(const char* text) {
    return text ? std::string(text) : std::string("?");
}

int runBanner(const AtlifmInvocation* inv, void* user) {
    const auto* api = static_cast<const AtlifmHostApi*>(user);
    std::string out = "atlifm " + quote(api->host_version()) + "\n";
    out += "cwd: " + quote(inv->cwd) + "\n";
    out += "selected: " + std::to_string(inv->selection_count) + "\n";
    api->output_write(out.c_str());
    const size_t limit = inv->selection_count < size_t{64} ? inv->selection_count : size_t{64};
    for (size_t i = 0; i < limit; ++i) {
        api->output_write("  ");
        api->output_write(inv->selection[i]);
        api->output_write("\n");
    }
    api->notify("hello: banner ready");
    return 0;
}

int overrideProperties(const AtlifmInvocation* inv, void*) {
    pluginLog("override hook: " + quote(inv->action) + " cwd=" + quote(inv->cwd) +
              " selection=" + std::to_string(inv->selection_count) + " -> fallback");
    return 0; // not handled: atlifm runs the builtin properties
}

void renderHello(const char* path, AtlifmWriteFn write, void* write_user, void*) {
    pluginLog("render: " + quote(path));
    FILE* f = path ? std::fopen(path, "r") : nullptr;
    if (!f) {
        const std::string message = "[hello plugin] cannot open " + quote(path) + "\n";
        write(write_user, message.c_str(), message.size());
        return;
    }
    const std::string header = "[hello plugin] " + quote(path) + "\n\n";
    write(write_user, header.c_str(), header.size());
    char line[1024];
    int n = 0;
    while (n < 200 && std::fgets(line, sizeof line, f)) {
        write(write_user, line, std::strlen(line));
        ++n;
    }
    std::fclose(f);
    const std::string footer = "\n-- end (" + std::to_string(n) + " lines) --\n";
    write(write_user, footer.c_str(), footer.size());
}

void hookOnStart(const AtlifmInvocation* inv, void*) {
    pluginLog("hook " + quote(inv->event) + ": cwd=" + quote(inv->cwd));
}

}

extern "C" int atlifm_plugin_init(const AtlifmHostApi* api) {
    if (!api || api->abi_version != ATLIFM_PLUGIN_ABI_VERSION ||
        api->struct_size < static_cast<uint32_t>(sizeof(AtlifmHostApi))) {
        return 1;
    }
    pluginLog("init: entry");

    auto* context = const_cast<AtlifmHostApi*>(api);

    AtlifmActionDesc action{};
    action.struct_size = static_cast<uint32_t>(sizeof(AtlifmActionDesc));
    action.id = "hello_banner";
    action.name = "Hello: show context";
    action.description = "Example native plugin: cwd and selection in the output overlay";
    action.keys = "Ctrl+Alt+H";
    action.applies_to = "any";
    action.user = context;
    action.run = runBanner;

    const int rcAction = api->register_action(&action);
    const int rcOverride = api->register_override("properties", overrideProperties, context);
    const int rcPreview = api->register_preview(".hello", renderHello, context);
    const int rcHook = api->register_hook("on_start", hookOnStart, context);

    pluginLog("init: action=" + std::to_string(rcAction) + " override=" +
              std::to_string(rcOverride) + " preview=" + std::to_string(rcPreview) +
              " hook=" + std::to_string(rcHook));
    api->log("info", "hello plugin initialized");
    return (rcAction == 0 && rcPreview == 0) ? 0 : 1;
}

extern "C" void atlifm_plugin_shutdown(void) {
    pluginLog("shutdown");
}
