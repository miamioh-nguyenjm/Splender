#pragma once

// globals.h


#include <atomic>

// Primary global used to indicate a background load is active.
extern std::atomic<bool> isLoading;
