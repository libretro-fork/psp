#pragma once


// Note that the string pointed to must have a lifetime until the end of the thread,
// for AssertCurrentThreadName to work.
void SetCurrentThreadName(const char *threadName);
void AssertCurrentThreadName(const char *threadName);

// If TLS is not supported, this will return an empty string.
const char *GetCurrentThreadName();

// Just gets a cheap thread identifier so that you can see different threads in debug output,
// exactly what it is is badly specified and not useful for anything.
int GetCurrentThreadIdForDebug();
