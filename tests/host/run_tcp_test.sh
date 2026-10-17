#!/bin/sh
# Run test_tcp in a throwaway, unprivileged network namespace (needs no root): see test_tcp.c.
exec unshare -rn "$(dirname "$0")/test_tcp" "$@"
