#pragma once

#include <corecrt.h>

#pragma warning(push)
#pragma warning(disable : _UCRT_DISABLED_WARNINGS)
_UCRT_DISABLE_CLANG_WARNINGS

_CRT_BEGIN_C_HEADER

#undef vertify

_ACRTIMP void __cdecl _wassert(_In_z_ wchar_t const* _Message,
  _In_z_ wchar_t const* _File, _In_ unsigned _Line);

#	define vertify(expression)                                           \
		(void)((!!(expression))                                          \
			   || (_wassert(_CRT_WIDE(#expression), _CRT_WIDE(__FILE__), \
					 (unsigned)(__LINE__)),                              \
				 0))

_CRT_END_C_HEADER
_UCRT_RESTORE_CLANG_WARNINGS
#pragma warning(pop)  // _UCRT_DISABLED_WARNINGS
