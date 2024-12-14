# Changelog #
All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](http://keepachangelog.com/)
and this project adheres to [Semantic Versioning](http://semver.org/).

## [Unreleased] ##

### Fixed ###
 * `catatonit` will now `chdir` to `/` in order to avoid keeping the directory
   it was running in busy. This was causing issues with rootless Podman's pause
   container keeping some `/home/...` paths busy and blocking `umount`s. (#28,
   #33)

## [0.2.0] - 2023-10-02

### Changed ###
 * `catatonit` has now been relicensed to GPLv2-or-later. (#27)

## [0.1.7] - 2021-11-01

### Added ###
 * Running `catatonit` as the only process in a pause container (i.e. no
   children are spawned, `catatonit` just runs in a busy loop) is now supported
   in the form of `catatonit -P`.

## [0.1.6] - 2021-09-16

### Fixed ###
 * File descriptors passed to the pid2 process were not closed by `catatonit`
   after starting pid2 which could lead to several negative scenarios (such as
   resources being kept around unnecessarily). Now `catatonit` will close all
   `>=3` (i.e. non-stdio) file descriptors after spawning the child process.
   (#12, #14)

 * Container-based socket activation support with runc sets `LISTEN_PID=1` when
   forwarding socket-activation-related file descriptors, but with `catatonit`
   this doesn't work because `catatonit` is pid1 not the actual container
   process. As such, `catatonit` will now rewrite the `LISTEN_PIDS` environment
   variable to equal the pid2 pid if `LISTEN_PIDS` is equal to the pid of
   `catatonit`. (#13, #15)

## [0.1.5] - 2020-03-03

### Fixed ###
 * Some cases where catatonit could hang if pid1 died and the death signal was
   coalesced are now correctly handled. (#4)

## [0.1.4] - 2019-01-29

### Added ###
 * We now support the `-g` option (from `tini`) for signals to be sent to the
   process group. This is necessary for Rook to switch to `catatonit`.

## [0.1.3] - 2018-04-18

### Fixed ##
 * Improve Docker compatibility by reporting ourselves as `tini` when providing
   version information from `catatonit -V`.

## [0.1.2] - 2018-03-29

### Changed ###
 * Minor cosmetic changes, as we are now an openSUSE project.

## [0.1.1] - 2018-03-27

### Fixed ###
 * Add a small fix for the libtool requirements to allow building catatonit on
   older distributions.

## 0.1.0 - 2018-03-27

This is the first release of catatonit. At this point it works fully (to
the best of my ability) and is incredibly simple to use and maintain.

[Unreleased]: https://github.com/openSUSE/catatonit/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/openSUSE/catatonit/compare/v0.1.7...v0.2.0
[0.1.7]: https://github.com/openSUSE/catatonit/compare/v0.1.6...v0.1.7
[0.1.6]: https://github.com/openSUSE/catatonit/compare/v0.1.5...v0.1.6
[0.1.5]: https://github.com/openSUSE/catatonit/compare/v0.1.4...v0.1.5
[0.1.4]: https://github.com/openSUSE/catatonit/compare/v0.1.3...v0.1.4
[0.1.3]: https://github.com/openSUSE/catatonit/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/openSUSE/catatonit/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/openSUSE/catatonit/compare/v0.1.0...v0.1.1
