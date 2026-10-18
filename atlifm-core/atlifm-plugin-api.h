/* atlifm-plugin-api.h: C ABI нативных in-process плагинов atlifm, версия 1.
 *
 * плагин это *.so в ~/.config/atlifm/plugins/, экспортирующий
 * atlifm_plugin_init; хост грузит через dlopen и передаёт AtlifmHostApi,
 * регистрации только во время init, потом таблица неизменна.
 *
 * потоки: run/override/hook в GUI потоке, render превью в рабочем потоке
 * (Qt thread pool), там только write и своя логика, остальные вызовы
 * хоста запрещены. идентификаторы с "sudo" перехвату не подлежат,
 * sudo/polkit/portal вообще не проходят через диспетчер плагинов. */
#ifndef ATLIFM_PLUGIN_API_H
#define ATLIFM_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ATLIFM_PLUGIN_ABI_VERSION 1u

typedef struct AtlifmHostApi AtlifmHostApi;

/* контекст вызова: event (hook), action (id действия/override), cwd и
 * выделенные пути. Строки в UTF-8, живут только на время вызова */
typedef struct AtlifmInvocation {
    uint32_t struct_size;
    const char* event;
    const char* action;
    const char* cwd;
    const char* const* selection;
    size_t selection_count;
} AtlifmInvocation;

/* приёмник текста: preview передаёт сюда куски вывода (write_user это
 * указатель из регистрации) */
typedef void (*AtlifmWriteFn)(void* write_user, const char* data, size_t len);

/* описание действия (пункт меню Plugins + биндинг через Keys):
 * id обязателен [a-z0-9_], первая буква; name подпись в меню;
 * applies_to "any"/"files"/"dirs", extensions ".md,.log" (файлы);
 * enabled опционален, <=0 скрывает пункт; run: 0 успех */
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
} AtlifmActionDesc;

/* override: 1 событие обработано, 0 откат к следующему плагину или
 * встроенному поведению. Перехватываются все действия atlifm
 * (клавиши, меню, мышь), кроме sudo/polkit/portal */
typedef int (*AtlifmOverrideFn)(const AtlifmInvocation* inv, void* user);

/* preview: render получает путь файла и write для текста; пустой вывод
 * это откат к встроенному превью. extensions обязательны */
typedef void (*AtlifmPreviewFn)(const char* path, AtlifmWriteFn write,
                                void* write_user, void* user);

/* hook: события on_start / on_navigate / on_select */
typedef void (*AtlifmHookFn)(const AtlifmInvocation* inv, void* user);

struct AtlifmHostApi {
    uint32_t abi_version;
    uint32_t struct_size;

    const char* (*host_version)(void);
    /* текст в оверлей текущего действия (нижний правый угол), доступно
     * в run/override, в hook оверлея нет */
    void (*output_write)(const char* text);
    /* короткое сообщение в статусбар */
    void (*notify)(const char* message);
    /* запуск внешней команды: detach=1 отдельный процесс, detach=0 вывод
     * стримится в оверлей действия; 0 ok */
    int (*run_command)(const char* command_line, int detach);
    /* перечитать списки файлов и закладки после изменений на диске */
    void (*refresh)(void);

    int (*register_action)(const AtlifmActionDesc* desc);
    int (*register_override)(const char* action_id, AtlifmOverrideFn run, void* user);
    int (*register_preview)(const char* extensions, AtlifmPreviewFn render, void* user);
    int (*register_hook)(const char* event, AtlifmHookFn fn, void* user);
    void (*log)(const char* level, const char* message);
};

/* init вернуть 0 при успехе; при несовпадении api->abi_version abort и
 * не загружать; shutdown опционален */
int atlifm_plugin_init(const AtlifmHostApi* api);
void atlifm_plugin_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
