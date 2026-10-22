/* atlifm example native plugin using C ABI version 1
 *
 * build instructions:
 * compile this file as a shared library targeting atlifm core headers
 * for example using your c plus plus compiler with shared library options and output name hello.so
 *
 * installation instructions:
 * copy the compiled hello.so library into the user plugins directory inside atlifm config folder
 *
 * features demonstrated by this plugin:
 * 1. hello_banner custom menu action inside plugins menu bound to Ctrl+Alt+H
 * 2. properties action override returning zero which triggers fallback to default properties dialog
 * 3. custom text preview renderer for hello files showing first few lines
 * 4. startup lifecycle hook logging current working directory
 *
 * log destination:
 * logs are appended to temporary file atlifm_hello.log or path from ATLIFM_PLUGIN_LOG environment variable
 */
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
    return 0; // returning zero lets atlifm continue with builtin properties handler
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
