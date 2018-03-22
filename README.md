## catatonit ##

A container init that is so simple it's effectively brain-dead. This is a
rewrite of [initrs][initrs] in C, because we found that it is not possible to
statically compile Rust binaries without using musl. That was, in turn, a
reimplementation of other container inits like `tini` and `dumb-init` which
have some concerning implementation details that are non-trivial to just patch
out.

[initrs]: https://github.com/cyphar/initrs
