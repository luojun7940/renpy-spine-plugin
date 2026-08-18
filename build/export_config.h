/* Force-included when building spine-c as a DLL:
 * export all SP_API functions via SPINEPLUGIN_API. */
#ifdef _WIN32
#define SPINEPLUGIN_API __declspec(dllexport)
#endif
