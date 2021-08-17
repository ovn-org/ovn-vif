# -*- autoconf -*-

# Copyright (c) 2008-2016, 2019 Nicira, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at:
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
m4_include([m4/compat.m4])

dnl Checks for --enable-ndebug and defines NDEBUG if it is specified.
AC_DEFUN([OVS_CHECK_NDEBUG],
  [AC_ARG_ENABLE(
     [ndebug],
     [AC_HELP_STRING([--enable-ndebug],
                     [Disable debugging features for max performance])],
     [case "${enableval}" in
        (yes) ndebug=true ;;
        (no)  ndebug=false ;;
        (*) AC_MSG_ERROR([bad value ${enableval} for --enable-ndebug]) ;;
      esac],
     [ndebug=false])
   AM_CONDITIONAL([NDEBUG], [test x$ndebug = xtrue])])

dnl Checks for valgrind/valgrind.h.
AC_DEFUN([OVN_CHECK_VALGRIND],
  [AC_CHECK_HEADERS([valgrind/valgrind.h])])

dnl Checks for thread-local storage support.
dnl
dnl Checks whether the compiler and linker support the C11
dnl thread_local macro from <threads.h>, and if so defines
dnl HAVE_THREAD_LOCAL.  If not, checks whether the compiler and linker
dnl support the GCC __thread extension, and if so defines
dnl HAVE___THREAD.
AC_DEFUN([OVS_CHECK_TLS],
  [AC_CACHE_CHECK(
     [whether $CC has <threads.h> that supports thread_local],
     [ovs_cv_thread_local],
     [AC_LINK_IFELSE(
        [AC_LANG_PROGRAM([#include <threads.h>
static thread_local int var;], [return var;])],
        [ovs_cv_thread_local=yes],
        [ovs_cv_thread_local=no])])
   if test $ovs_cv_thread_local = yes; then
     AC_DEFINE([HAVE_THREAD_LOCAL], [1],
               [Define to 1 if the C compiler and linker supports the C11
                thread_local matcro defined in <threads.h>.])
   else
     AC_CACHE_CHECK(
       [whether $CC supports __thread],
       [ovs_cv___thread],
       [AC_LINK_IFELSE(
          [AC_LANG_PROGRAM([static __thread int var;], [return var;])],
          [ovs_cv___thread=yes],
          [ovs_cv___thread=no])])
     if test $ovs_cv___thread = yes; then
       AC_DEFINE([HAVE___THREAD], [1],
                 [Define to 1 if the C compiler and linker supports the
                  GCC __thread extenions.])
     fi
   fi])

dnl OVS_CHECK_ATOMIC_LIBS
dnl
dnl Check to see if -latomic is need for GCC atomic built-ins.
AC_DEFUN([OVS_CHECK_ATOMIC_LIBS],
   [AC_SEARCH_LIBS([__atomic_load_8], [atomic])])

dnl OVS_CHECK_GCC4_ATOMICS
dnl
dnl Checks whether the compiler and linker support GCC 4.0+ atomic built-ins.
dnl A compile-time only check is not enough because the compiler defers
dnl unimplemented built-ins to libgcc, which sometimes also lacks
dnl implementations.
AC_DEFUN([OVS_CHECK_GCC4_ATOMICS],
  [AC_CACHE_CHECK(
     [whether $CC supports GCC 4.0+ atomic built-ins],
     [ovs_cv_gcc4_atomics],
     [AC_LINK_IFELSE(
        [AC_LANG_PROGRAM([[#include <stdlib.h>

#define ovs_assert(expr) if (!(expr)) abort();
#define TEST_ATOMIC_TYPE(TYPE)                  \
    {                                           \
        TYPE x = 1;                             \
        TYPE orig;                              \
                                                \
        __sync_synchronize();                   \
        ovs_assert(x == 1);                     \
                                                \
        __sync_synchronize();                   \
        x = 3;                                  \
        __sync_synchronize();                   \
        ovs_assert(x == 3);                     \
                                                \
        orig = __sync_fetch_and_add(&x, 1);     \
        ovs_assert(orig == 3);                  \
        __sync_synchronize();                   \
        ovs_assert(x == 4);                     \
                                                \
        orig = __sync_fetch_and_sub(&x, 2);     \
        ovs_assert(orig == 4);                  \
        __sync_synchronize();                   \
        ovs_assert(x == 2);                     \
                                                \
        orig = __sync_fetch_and_or(&x, 6);      \
        ovs_assert(orig == 2);                  \
        __sync_synchronize();                   \
        ovs_assert(x == 6);                     \
                                                \
        orig = __sync_fetch_and_and(&x, 10);    \
        ovs_assert(orig == 6);                  \
        __sync_synchronize();                   \
        ovs_assert(x == 2);                     \
                                                \
        orig = __sync_fetch_and_xor(&x, 10);    \
        ovs_assert(orig == 2);                  \
        __sync_synchronize();                   \
        ovs_assert(x == 8);                     \
    }]], [dnl
TEST_ATOMIC_TYPE(char);
TEST_ATOMIC_TYPE(unsigned char);
TEST_ATOMIC_TYPE(signed char);
TEST_ATOMIC_TYPE(short);
TEST_ATOMIC_TYPE(unsigned short);
TEST_ATOMIC_TYPE(int);
TEST_ATOMIC_TYPE(unsigned int);
TEST_ATOMIC_TYPE(long int);
TEST_ATOMIC_TYPE(unsigned long int);
TEST_ATOMIC_TYPE(long long int);
TEST_ATOMIC_TYPE(unsigned long long int);
])],
        [ovs_cv_gcc4_atomics=yes],
        [ovs_cv_gcc4_atomics=no])])
   if test $ovs_cv_gcc4_atomics = yes; then
     AC_DEFINE([HAVE_GCC4_ATOMICS], [1],
               [Define to 1 if the C compiler and linker supports the GCC 4.0+
                atomic built-ins.])
   fi])

dnl OVS_CHECK_ATOMIC_ALWAYS_LOCK_FREE(SIZE)
dnl
dnl Checks __atomic_always_lock_free(SIZE, 0)
AC_DEFUN([OVS_CHECK_ATOMIC_ALWAYS_LOCK_FREE],
  [AC_CACHE_CHECK(
    [value of __atomic_always_lock_free($1)],
    [ovs_cv_atomic_always_lock_free_$1],
    [AC_COMPUTE_INT(
        [ovs_cv_atomic_always_lock_free_$1],
        [__atomic_always_lock_free($1, 0)],
        [],
        [ovs_cv_atomic_always_lock_free_$1=unsupported])])
   if test ovs_cv_atomic_always_lock_free_$1 != unsupported; then
     AC_DEFINE_UNQUOTED(
       [ATOMIC_ALWAYS_LOCK_FREE_$1B],
       [$ovs_cv_atomic_always_lock_free_$1],
       [If the C compiler is GCC 4.7 or later, define to the return value of
        __atomic_always_lock_free($1, 0).  If the C compiler is not GCC or is
        an older version of GCC, the value does not matter.])
   fi])

dnl OVS_CHECK_POSIX_AIO
AC_DEFUN([OVS_CHECK_POSIX_AIO],
  [AC_SEARCH_LIBS([aio_write], [rt])
   AM_CONDITIONAL([HAVE_POSIX_AIO], [test "$ac_cv_search_aio_write" != no])])

dnl OVS_CHECK_INCLUDE_NEXT
AC_DEFUN([OVS_CHECK_INCLUDE_NEXT],
  [AC_REQUIRE([gl_CHECK_NEXT_HEADERS])
   gl_CHECK_NEXT_HEADERS([$1])])

dnl OVS_CHECK_PRAGMA_MESSAGE
AC_DEFUN([OVS_CHECK_PRAGMA_MESSAGE],
  [AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
   [[_Pragma("message(\"Checking for pragma message\")")
   ]])],
     [AC_DEFINE(HAVE_PRAGMA_MESSAGE,1,[Define if compiler supports #pragma
     message directive])])
  ])

dnl OVS_LIBTOOL_VERSIONS sets the major, minor, micro version information for
dnl OVS_LTINFO variable.  This variable locks libtool information for shared
dnl objects, allowing multiple versions with different ABIs to coexist.
AC_DEFUN([OVS_LIBTOOL_VERSIONS],
    [AC_MSG_CHECKING(linker output version information)
  OVS_MAJOR=`echo "$PACKAGE_VERSION" | sed -e 's/[[.]].*//'`
  OVS_MINOR=`echo "$PACKAGE_VERSION" | sed -e "s/^$OVS_MAJOR//" -e 's/^.//' -e 's/[[.]].*//'`
  OVS_MICRO=`echo "$PACKAGE_VERSION" | sed -e "s/^$OVS_MAJOR.$OVS_MINOR//" -e 's/^.//' -e 's/[[^0-9]].*//'`
  OVS_LT_RELINFO="-release $OVS_MAJOR.$OVS_MINOR"
  OVS_LT_VERINFO="-version-info $LT_CURRENT:$OVS_MICRO"
  OVS_LTINFO="$OVS_LT_RELINFO $OVS_LT_VERINFO"
  AC_MSG_RESULT([libX-$OVS_MAJOR.$OVS_MINOR.so.$LT_CURRENT.0.$OVS_MICRO)])
  AC_SUBST(OVS_LTINFO)
    ])

dnl OVS does not use C++ itself, but it provides public header files
dnl that a C++ compiler should accept, so when --enable-Werror is in
dnl effect and a C++ compiler is available, we enable building a C++
dnl source file that #includes all the public headers, as a way to
dnl ensure that they are acceptable as C++.
AC_DEFUN([OVS_CHECK_CXX],
  [AC_REQUIRE([AC_PROG_CXX])
   AC_REQUIRE([OVS_ENABLE_WERROR])
   AX_CXX_COMPILE_STDCXX([11], [], [optional])
   if test $enable_Werror = yes && test $HAVE_CXX11 = 1; then
     enable_cxx=:
     AC_LANG_PUSH([C++])
     AC_CHECK_HEADERS([atomic])
     AC_LANG_POP([C++])
   else
     enable_cxx=false
   fi
   AM_CONDITIONAL([HAVE_CXX], [$enable_cxx])])
