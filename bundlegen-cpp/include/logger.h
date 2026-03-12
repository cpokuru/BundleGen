// If not stated otherwise in this file or this component's license file the
// following copyright and licenses apply:
//
// Copyright 2024 Consult Red
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once
#include <cstdio>
#include <cstring>

enum class LogLevel { TRACE=0, DEBUG=1, INFO=2, WARNING=3, ERROR=4, SUCCESS=5, NONE=6 };

// Global log level - set from CLI
extern LogLevel g_logLevel;

#define LOG_ERROR(fmt, ...)   do { if(g_logLevel <= LogLevel::ERROR)   fprintf(stderr, "[ERROR]   " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_WARNING(fmt, ...) do { if(g_logLevel <= LogLevel::WARNING) fprintf(stderr, "[WARNING] " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_INFO(fmt, ...)    do { if(g_logLevel <= LogLevel::INFO)    fprintf(stderr, "[INFO]    " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_DEBUG(fmt, ...)   do { if(g_logLevel <= LogLevel::DEBUG)   fprintf(stderr, "[DEBUG]   " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_TRACE(fmt, ...)   do { if(g_logLevel <= LogLevel::TRACE)   fprintf(stderr, "[TRACE]   " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_SUCCESS(fmt, ...) do { if(g_logLevel <= LogLevel::SUCCESS) fprintf(stderr, "[SUCCESS] " fmt "\n", ##__VA_ARGS__); } while(0)
