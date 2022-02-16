/* ssh_ping.c */
/*
Copyright 2018 Red Hat, Inc

Author: Jakub Jelen <jjelen@redhat.com>

This file is part of the SSH Library

You are free to copy this file, modify it in any way, consider it being public
domain. This does not apply to the rest of the library though, but it is
allowed to cut-and-paste working code from this file to any license of
program.
The goal is to show the API in action. It's not a reference on how terminal
clients must be made or how a client should react.
*/

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <libssh/libssh.h>
#include <libssh/kex.h>
#include <libssh/session.h>

int main(int argc, char **argv)
{
    const char *banner = NULL;
    ssh_session session = NULL;
    const char *hostkeys = NULL;
    int rc = 1;
    int port;
    int level = 5;

    bool process_config = false;

    if (argc < 1 || argv[1] == NULL) {
        fprintf(stderr, "Error: Need an argument (hostname)\n");
        goto out;
    }

    if (argc < 2 || argv[2] == NULL) {
        port = 22;
    } else {
        port = atoi(argv[2]);
        if (port <= 0) {
            fprintf(stderr, "Port is not valid: %s\n", argv[2]);
            goto out;
        }
    }

    ssh_init();

    session = ssh_new();
    if (session == NULL) {
        goto out;
    }

    rc = ssh_options_set(session, SSH_OPTIONS_HOST, argv[1]);
    if (rc < 0) {
        goto out;
    }

    rc = ssh_options_set(session, SSH_OPTIONS_PORT, &port);
    if (rc < 0) {
        goto out;

    }

    /* The automatic username is not available under uid wrapper */
    rc = ssh_options_set(session, SSH_OPTIONS_USER, "ping");
    if (rc < 0) {
        goto out;
    }

    /* Ignore system-wide configurations when simply trying to reach host */
    rc = ssh_options_set(session, SSH_OPTIONS_PROCESS_CONFIG, &process_config);
    if (rc < 0) {
        goto out;
    }

    rc = ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &level);
    if (rc < 0) {
        goto out;
    }

    /* Enable all supported algorithms (including DSA) */
    hostkeys = ssh_kex_get_supported_method(SSH_HOSTKEYS);
    rc = ssh_options_set(session, SSH_OPTIONS_HOSTKEYS, hostkeys);
    if (rc < 0) {
        goto out;
    }

    rc = ssh_connect(session);
    if (rc != SSH_OK) {
        fprintf(stderr, "Connection failed : %s\n", ssh_get_error(session));
        goto out;
    }

    banner = ssh_get_serverbanner(session);
    if (banner == NULL) {
        fprintf(stderr, "Did not receive SSH banner\n");
        goto out;
    }

    printf("OK: %s\n", banner);
    rc = 0;

out:
    ssh_free(session);
    ssh_finalize();
    return rc;
}

