#pragma once

// Platform-specific definitions of a numeric thread ID type and an
// invalid value
namespace moodycamel {
	namespace details {
		template<typename thread_id_t> struct thread_id_converter {
			typedef thread_id_t thread_id_numeric_size_t;
			typedef thread_id_t thread_id_hash_t;
			static thread_id_hash_t prehash(thread_id_t const& x) { return x; }
		};
	}
}

/// ---- thread id stuff ----
#if defined(_WIN32) || defined(__WINDOWS__) || defined(__WIN32__)
// No sense pulling in windows.h in a header, we'll manually declare the
// function we use and rely on backwards-compatibility for this not to break
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(
  void);
namespace moodycamel {
	namespace details {
		static_assert(sizeof(unsigned long) == sizeof(std::uint32_t),
		  "Expected size of unsigned long to be 32 bits on Windows");
		typedef std::uint32_t thread_id_t;
		static const thread_id_t invalid_thread_id =
		  0;  // See
			  // http://blogs.msdn.com/b/oldnewthing/archive/2004/02/23/78395.aspx
		static const thread_id_t invalid_thread_id2 =
		  0xFFFFFFFFU;	// Not technically guaranteed to be invalid, but is
						// never used in practice. Note that all Win32 thread
						// IDs are presently multiples of 4.
		static inline thread_id_t thread_id() {
			return static_cast<thread_id_t>(::GetCurrentThreadId());
		}
	}
}
#elif defined(__arm__) || defined(_M_ARM) || defined(__aarch64__) \
  || (defined(__APPLE__) && TARGET_OS_IPHONE)                     \
  || defined(MOODYCAMEL_NO_THREAD_LOCAL)
namespace moodycamel {
	namespace details {
		static_assert(
		  sizeof(std::thread::id) == 4 || sizeof(std::thread::id) == 8,
		  "std::thread::id is expected to be either 4 or 8 bytes");

		typedef std::thread::id thread_id_t;
		static const thread_id_t
		  invalid_thread_id;  // Default ctor creates invalid ID

		// Note we don't define a invalid_thread_id2 since std::thread::id
		// doesn't have one; it's only used if
		// MOODYCAMEL_CPP11_THREAD_LOCAL_SUPPORTED is defined anyway, which it
		// won't be.
		static inline thread_id_t thread_id() {
			return std::this_thread::get_id();
		}

		template<std::size_t> struct thread_id_size {};
		template<> struct thread_id_size<4> {
			typedef std::uint32_t numeric_t;
		};
		template<> struct thread_id_size<8> {
			typedef std::uint64_t numeric_t;
		};

		template<> struct thread_id_converter<thread_id_t> {
			typedef thread_id_size<sizeof(thread_id_t)>::numeric_t
			  thread_id_numeric_size_t;
#	ifndef __APPLE__
			typedef std::size_t thread_id_hash_t;
#	else
			typedef thread_id_numeric_size_t thread_id_hash_t;
#	endif

			static thread_id_hash_t prehash(thread_id_t const& x) {
#	ifndef __APPLE__
				return std::hash<std::thread::id>()(x);
#	else
				return *reinterpret_cast<thread_id_hash_t const*>(&x);
#	endif
			}
		};
	}
}
#else
// Use a nice trick from this answer: http://stackoverflow.com/a/8438730/21475
// In order to get a numeric thread ID in a platform-independent way, we use a
// thread-local static variable's address as a thread identifier :-)
#	if defined(__GNUC__) || defined(__INTEL_COMPILER)
#		define MOODYCAMEL_THREADLOCAL __thread
#	elif defined(_MSC_VER)
#		define MOODYCAMEL_THREADLOCAL __declspec(thread)
#	else
// Assume C++11 compliant compiler
#		define MOODYCAMEL_THREADLOCAL thread_local
#	endif
namespace moodycamel {
	namespace details {
		typedef std::uintptr_t thread_id_t;
		static const thread_id_t invalid_thread_id =
		  0;  // Address can't be nullptr
		static const thread_id_t invalid_thread_id2 =
		  1;  // Member accesses off a null pointer are also generally invalid.
			  // Plus it's not aligned.
		inline thread_id_t thread_id() {
			static MOODYCAMEL_THREADLOCAL int x;
			return reinterpret_cast<thread_id_t>(&x);
		}
	}
}
#endif

// Constexpr if
#define MOODYCAMEL_CONSTEXPR_IF if constexpr
#define MOODYCAMEL_MAYBE_UNUSED [[maybe_unused]]

// Exceptions
#ifndef MOODYCAMEL_EXCEPTIONS_ENABLED
#	ifdef __EXCEPTIONS
#		define MOODYCAMEL_EXCEPTIONS_ENABLED
#	endif
#endif
#ifdef MOODYCAMEL_EXCEPTIONS_ENABLED
#	define MOODYCAMEL_TRY try
#	define MOODYCAMEL_CATCH(...) catch (__VA_ARGS__)
#	define MOODYCAMEL_RETHROW throw
#	define MOODYCAMEL_THROW(expr) throw(expr)
#else
#	define MOODYCAMEL_TRY MOODYCAMEL_CONSTEXPR_IF(true)
#	define MOODYCAMEL_CATCH(...) else MOODYCAMEL_CONSTEXPR_IF(false)
#	define MOODYCAMEL_RETHROW
#	define MOODYCAMEL_THROW(expr)
#endif

#define MOODYCAMEL_NOEXCEPT noexcept
#define MOODYCAMEL_NOEXCEPT_CTOR(type, valueType, expr) noexcept(expr)
#define MOODYCAMEL_NOEXCEPT_ASSIGN(type, valueType, expr) noexcept(expr)


// VS2013 doesn't support `thread_local`, and MinGW-w64 w/ POSIX threading has a
// crippling bug: http://sourceforge.net/p/mingw-w64/bugs/445 g++ <=4.7 doesn't
// support thread_local either. Finally, iOS/ARM doesn't have support for it
// either, and g++/ARM allows it to compile but it's unconfirmed to actually
// work
//#define MOODYCAMEL_CPP11_THREAD_LOCAL_SUPPORTED    // always disabled for now
// since several users report having problems with it on


// VS2012 doesn't support deleted functions.
// In this case, we declare the function normally but don't define it. A link
// error will be generated if the function is called.
#define MOODYCAMEL_DELETE_FUNCTION = delete

// Alignas() Alignof() stuff
namespace moodycamel {
	namespace details {
		template<typename T> struct identity { typedef T type; };
#define MOODYCAMEL_ALIGNAS(alignment) alignas(alignment)
#define MOODYCAMEL_ALIGNOF(obj) alignof(obj)
#define MOODYCAMEL_ALIGNED_TYPE_LIKE(T, obj) \
	alignas(alignof(obj)) typename details::identity<T>::type
	}
}

// TSAN can false report races in lock-free code.  To enable TSAN to be used
// from projects that use this one, we can apply per-function compile-time
// suppression. See
// https://clang.llvm.org/docs/ThreadSanitizer.html#has-feature-thread-sanitizer
#define MOODYCAMEL_NO_TSAN
#if defined(__has_feature)
#	if __has_feature(thread_sanitizer)
#		undef MOODYCAMEL_NO_TSAN
#		define MOODYCAMEL_NO_TSAN __attribute__((no_sanitize("thread")))
#	endif	// TSAN
#endif		// TSAN

// Change????
// Compiler-specific likely/unlikely hints
namespace moodycamel {
	namespace details {
#if defined(__GNUC__)
		static inline bool(likely)(bool x) {
			return __builtin_expect((x), true);
		}
		static inline bool(unlikely)(bool x) {
			return __builtin_expect((x), false);
		}
#else
		static inline bool(likely)(bool x) { return x; }
		static inline bool(unlikely)(bool x) { return x; }
#endif
	}
}

#ifdef MOODYCAMEL_QUEUE_INTERNAL_DEBUG
#	include "internal/concurrentqueue_internal_debug.h"
#endif