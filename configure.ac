# catatonit: a container init so simple it's effectively brain-dead
# Copyright (C) 2018 SUSE LLC
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

AC_PREREQ([2.69])
AC_INIT([catatonit], [0.1.2], [https://bugs.opensuse.org/], [], [https://github.com/openSUSE/catatonit/])
AM_INIT_AUTOMAKE([-Wall foreign])

LT_PREREQ([2.4.2])
LT_INIT([disable-shared])

AC_CHECK_HEADERS([errno.h fcntl.h signal.h stdarg.h stdio.h stdlib.h unistd.h])
AC_CHECK_HEADERS([sys/prctl.h sys/signalfd.h sys/stat.h sys/types.h sys/wait.h])

AC_TYPE_PID_T
AC_FUNC_FORK

AC_CONFIG_FILES([Makefile config.h VERSION])
AC_OUTPUT
AM_INIT_AUTOMAKE
