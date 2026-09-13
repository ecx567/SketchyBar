#include <stdbool.h>

#define HOTLOAD_STATE_ENABLED true
#define HOTLOAD_STATE_DISABLED false

void exec_config_file();
void begin_receiving_config_change_events();
void hotload_set_state(int state);
int hotload_get_state();
bool set_config_file_path(char* file);

#ifdef _WIN32
// Windows (S5): the config watcher runs on a dedicated thread; this stops it
// and joins (2000 ms cap). Safe to call when watching was never started.
void hotload_stop_watching();
#endif
