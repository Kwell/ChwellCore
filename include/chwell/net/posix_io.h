#pragma once

// Compatibility umbrella for existing POSIX consumers. New code should include
// io_service.h, socket_io.h or posix_endpoint.h for the capability it needs.
#include "chwell/net/io_service.h"
#include "chwell/net/socket_io.h"
#include "chwell/net/posix_endpoint.h"
#include <vector>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
