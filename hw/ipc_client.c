/*-
 * Copyright (C) 2014 Nippon Telegraph and Telephone Corporation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>

#include "virtio.h"
#include "virtio-net.h"
#include "ipc_node.h"
#include "ipc_client.h"

static void set_link_status(VirtIODevice *vdev, int link_down)
{
	virtio_net_set_link_down(vdev, link_down);
}

static int init_client(VirtIODevice *vdev)
{
	ipc_node *c;
	int ram_fd = -1;
	ram_addr_t ram_size;
	int fd;
#if defined(__linux__) && !defined(TARGET_S390X)
	RAMBlock *block;

	/* find the addr of physical ram of guest */
	QLIST_FOREACH(block, &ram_list.blocks, next) {
		/* assume offset 0 is the physical memory */
		if (block->offset == 0) {
			ram_fd = block->fd;
			ram_size = block->length;
		}
	}
#endif

	if (ram_fd == -1) {
		perror("failed to find ram addr");
		return -EFAULT;
	}

	fd = ipc_socket();
	if (fd < 0) {
		perror("failed to open a socket");
		return -errno;
	}

	/* alloc */
	c = malloc(sizeof *c);
	if (!c) {
		perror("failed to malloc");
		return -EFAULT;
	}
	if (ipc_node_init(c,
			vdev->nid,
			fd, /*fd */
			ram_fd, /*ram_fd*/
			ram_size /* ram_size */)) {
		perror("failed to allocate a client");
		free(c);
		return -errno;
	}

	vdev->ipc = c;

	return 0;
}

static void ipc_set_connector(VirtIODevice *vdev)
{
	qemu_mod_timer(vdev->ts, qemu_get_clock_ms(rt_clock)
			+ (vdev->cinterval * 1000));
	return;
}

static void ipc_reader(void *opaque)
{
	VirtIODevice *vdev = (VirtIODevice *)opaque;
	ipc_node *c;
	uint16_t curq;
	VirtQueue *vq;
	ipc_msg_type mtype;
	int fd;

	if (!vdev)
		return;

	c = vdev->ipc;
	if (!c)
		return;

	if (ipc_client_recv(c, &mtype, &curq) < 0) {
		perror("can't receive data from a server");
		fd = ipc_get_fd(c);
		qemu_set_fd_handler(fd, NULL, NULL, NULL);
		shutdown(fd, SHUT_RDWR); /* ignore errors */
		set_link_status(vdev, 1);
		ipc_set_connector(vdev);
		return;
	}

	switch (mtype) {
		case IPC_MESSAGE_TYPE_KICK :
			vq = virtio_get_queue(vdev, curq);
			virtio_notify(vdev, vq);
			break;
		default:
			fprintf(stderr, "invalid ipc message type\n");
			break;
	}

	return;
}

static int connect_to_server(VirtIODevice *vdev)
{
	ipc_node *c = vdev->ipc;
	int fd, newfd;
	uint32_t lowmem_limit;

	/* open a socket */
	if ((fd = ipc_socket()) < 0) {
		perror("failed to open a socket");
		return -errno;
	}

	/* connect */
	if (ipc_connect(fd, (const char *)vdev->socketpath,
				strlen(vdev->socketpath))) {
		perror("failed to connect");
		goto err1;
	}

	/* First, send init message */

#ifdef QEMU_BELOW_4G_RAM_END
	lowmem_limit = QEMU_BELOW_4G_RAM_END;
#else
	lowmem_limit = 0xe0000000;
#endif
	if (ipc_client_init_sequence(c, lowmem_limit, fd) < 0) {
		perror("init sequence failed");
		goto err1;
	}

	/* Call dup2 after the init sequence. After the calling dup2,
	 * kick or other messages can be sent */
	newfd = ipc_get_fd(vdev->ipc);
	if (dup2(fd, newfd) < 0) {
		perror("dup2 failed");
		goto err1;
	}
	close(fd);
	fd = newfd;

	if (ipc_client_reconfigure(c) < 0) {
		perror("reconfigure failed");
		goto err2;
	}

	set_link_status(vdev, 0);

	qemu_set_fd_handler(fd, ipc_reader, NULL, vdev);

	return 0;

err1:
	close(fd);

	return -EFAULT;

err2:
	shutdown(fd, SHUT_RDWR); /* ignore errors */
	return -EFAULT;
}

static void ipc_connect_to_server(void *data)
{
	VirtIODevice *vdev = (VirtIODevice *)data;

	if (connect_to_server(vdev)) {
		/* try to connect to a server */
		ipc_set_connector(vdev);
	}
}

void ipc_init(VirtIODevice *vdev, char *socketpath, uint32_t nid,
	      uint32_t cinterval)
{
	vdev->socketpath = socketpath;
	vdev->nid = nid;
	vdev->cinterval = cinterval;
	if (init_client(vdev)) {
		fprintf(stderr,
			"Fatal error: can't init ipc node structure\n");
		return;
	}
	vdev->ts = qemu_new_timer_ms(rt_clock, ipc_connect_to_server, vdev);
	ipc_set_connector(vdev);
}

void ipc_exit(VirtIODevice *vdev)
{
	int fd;

	if (!vdev->ipc)
		return;
	qemu_del_timer(vdev->ts);
	qemu_free_timer(vdev->ts);
	fd = ipc_get_fd(vdev->ipc);
	qemu_set_fd_handler(fd, NULL, NULL, NULL);
	close(fd);
	set_link_status(vdev, 1);
	free(vdev->ipc);
	vdev->ipc = NULL;
}
