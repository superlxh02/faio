#pragma once
#include <string>
extern thread_local std::string windows_tls_second;

std::string windows_tls_value(const char* value);

bool windows_second_observes_stop();

int windows_second_task_value();
