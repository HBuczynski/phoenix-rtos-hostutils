/*
 * Phoenix-RTOS
 *
 * Phoenix server
 *
 * Copyright 2001, 2004 Pawel Pisarczyk
 * Copyright 2012 Phoenix Systems
 *
 * Author: Pawel Pisarczyk, Jacek Popko
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>
#include <stdlib.h>
#include <getopt.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <ctype.h>

#include "../common/types.h"
#include "../common/errors.h"
#include "../common/serial.h"
#include "../phoenixd/bsp.h"
#include "../phoenixd/msg_udp.h"
#include "../phoenixd/dispatch.h"

#include "../common/hid.h"

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))

/* SDP protocol section */
#define SET_CMD_TYPE(b,v) (b)[0]=(b)[1]=(v)
#define SET_ADDR(b,v) *((uint32_t*)((b)+2))=htonl(v)
#define SET_COUNT(b,v) *((uint32_t*)((b)+7))=htonl(v);
#define SET_DATA(b,v) *((uint32_t*)((b)+11))=htonl(v);
#define SET_FORMAT(b,v) (b)[6]=(v);

#define CMD_SIZE 17
#define BUF_SIZE 1025
#define INTERRUPT_SIZE 65

enum {
	SDP
};


typedef struct {
	int fd;
	unsigned int size;
	void *data;
} write_file_buff_t;


extern char *optarg;

static int sdp_writeRegister(hid_device *dev, uint32_t addr, uint8_t format, uint32_t data);
static int sdp_writeFile(hid_device *dev, uint32_t addr, void *data, size_t size);
static int sdp_dcdWrite(hid_device *dev, uint32_t addr, void *data, size_t size);
static int sdp_jmpAddr(hid_device *dev, uint32_t addr);
static int sdp_errStatus(hid_device *dev);

#define VERSION "1.3"


void usage(char *progname)
{
	printf("Usage: %s script_path\n", progname);
}


int phoenixd_session(char *tty, char *kernel, char *sysdir)
{
	u8 t;
	int fd, count, err;
	u8 buff[BSP_MSGSZ];

	fprintf(stderr, "[%d] Starting phoenixd-child on %s\n", getpid(), tty);

	if ((fd = serial_open(tty, B460800)) < 0) {
		fprintf(stderr, "[%d] Can't open %s [%d]!\n", getpid(), tty, fd);
		return ERR_PHOENIXD_TTY;
	}

	for (;;) {
		if ((count = bsp_recv(fd, &t, (char*)buff, BSP_MSGSZ, 0)) < 0) {
			bsp_send(fd, BSP_TYPE_RETR, NULL, 0);
			continue;
		}

		switch (t) {

		/* Handle kernel request */
		case BSP_TYPE_KDATA:
			if (*(u8 *)buff != 0) {
				fprintf(stderr, "[%d] Bad kernel request on %s\n", getpid(), tty);
				break;
			}
			fprintf(stderr, "[%d] Sending kernel to %s\n", getpid(), tty);

			if ((err = bsp_sendkernel(fd, kernel)) < 0) {
				fprintf(stderr, "[%d] Sending kernel error [%d]!\n", getpid(), err);
				break;
			}
			break;

		/* Handle program request */
		case BSP_TYPE_PDATA:
			fprintf(stderr, "[%d] Load program request on %s, program=%s\n", getpid(), tty, &buff[2]);
			if ((err = bsp_sendprogram(fd, (char*)&buff[2], sysdir)) < 0)
				fprintf(stderr, "[%d] Sending program error [%d]!\n", getpid(), err);
			break;
		}
	}
	return 0;
}


static inline void set_write_file_cmd(unsigned char* b, uint32_t addr, uint32_t size)
{
	SET_CMD_TYPE(b,0x04);
	SET_ADDR(b,addr);
	SET_COUNT(b,size);
	SET_FORMAT(b,0x20);
}


static inline void set_dcd_write_cmd(unsigned char* b, uint32_t addr, uint32_t size)
{
	SET_CMD_TYPE(b,0x0a);
	SET_ADDR(b,addr);
	SET_COUNT(b,size);
}


static inline void set_jmp_cmd(unsigned char* b, uint32_t addr)
{
	SET_CMD_TYPE(b,0x0b);
	SET_ADDR(b,addr);
	SET_FORMAT(b,0x20);
}


static inline void set_status_cmd(unsigned char* b)
{
	SET_CMD_TYPE(b,0x05);
}


static inline void set_write_reg_cmd(unsigned char* b, uint32_t addr, uint8_t format, uint32_t data)
{
	SET_CMD_TYPE(b,0x02);
	SET_ADDR(b,addr);
	SET_FORMAT(b,format);
	SET_DATA(b, data);
}


static inline int8_t char_to_hex(char c)
{
	c = tolower(c);
	if (c >= '0' && c <= '9') {
		return c - '0';
	} else if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}


int wait_cmd(hid_device **dev)
{
	if (*dev != NULL) {
		hid_close(*dev);
	}
	long int vid, pid;
	char *tok = strtok(NULL, " ");
	vid = strtol(tok, NULL, 0);
	tok = strtok(NULL, " ");
	pid = strtol(tok, NULL, 0);

	int retries = 0;
	sleep(1);
	while ((*dev = open_device(vid, pid)) == NULL) {
		if (retries++ > 10)
			return -1;
		sleep(1);
	}

	return 0;
}


int write_reg_cmd(hid_device *dev)
{
	long int addr, data, format;
	char *tok = strtok(NULL, " ");
	addr = strtol(tok, NULL, 0);
	tok = strtok(NULL, " ");
	data = strtol(tok, NULL, 0);
	tok = strtok(NULL, " ");
	format = strtol(tok, NULL, 0);

	return sdp_writeRegister(dev, addr, format, data);
}


int jump_addr_cmd(hid_device *dev)
{
	long int addr;
	char *tok = strtok(NULL, " ");
	addr = strtol(tok, NULL, 0);

	return sdp_jmpAddr(dev, addr);
}


int err_status_cmd(hid_device *dev)
{
	return sdp_errStatus(dev);
}


int parse_byte_string(const char *str, char **out)
{
	int res;
	size_t size = strlen(str);
	*out = malloc(size * sizeof(*str));
	char * const out_start = *out;
	const char * const str_end = str + size;
	--str;
	while ((++str) < str_end) {
		if (*str == '\\') {
			++str;
			if (*str == '\\') {
				*((*out)++) = '\\';
			} else if (*str == 'x') {
				*(*out)      = (char_to_hex(*(++str)) & 0xf) << 4;
				*((*out)++) |= (char_to_hex(*(++str)) & 0xf);
			} else {
				fprintf(stderr, "Malformed byte string passed\n");
				free(out_start);
				*out = NULL;
				return -1;
			}
		} else {
			*((*out)++) = *str;
		}
	}
	res = *out - out_start;
	*out = out_start;
	return res;
}


int get_buffer(char type, char *str, write_file_buff_t *buff)
{
	int err = -1;
	if (type == 'F') {
		struct stat statbuf;
		buff->fd = open(str, O_RDONLY);
		fstat(buff->fd, &statbuf);
		buff->size = statbuf.st_size;
		buff->data = mmap(NULL, statbuf.st_size, PROT_READ, MAP_PRIVATE, buff->fd, 0);
		if (buff->data != NULL)
			err = 0;
	} else if (type == 'S') {
		buff->size = parse_byte_string(str, (char **)&buff->data);
		if (buff->data != NULL)
			err = 0;
	}
	return err;
}


int close_buffer(char type, write_file_buff_t *buff)
{
	int err = -1;
	if (type == 'F') {
		close(buff->fd);
		err = munmap(buff->data, buff->size);
	} else if (type == 'S') {
		free(buff->data);
	}
	return err;
}


int write_file_cmd(hid_device *dev)
{
	long int offset = 0, addr = 0, size = 0;
	int res;
	write_file_buff_t buff;

	char *type = strtok(NULL, " ");
	char *str = strtok(NULL, "\"");
	char *tok = strtok(NULL, " ");
	if (tok != NULL) {
		 offset = strtol(tok, NULL, 0);
		 tok = strtok(NULL, " ");
	}
	if (tok != NULL) {
		 addr = strtol(tok, NULL, 0);
		 tok = strtok(NULL, " ");
	}
	if (tok != NULL) {
		 size = strtol(tok, NULL, 0);
	}

	if((res = get_buffer(*type, str, &buff)) < 0)
		return res;

	if (size) {
		size = MIN(size, buff.size);
	} else {
		size = buff.size;
	}
	res = sdp_writeFile(dev, addr, buff.data + offset, size);
	close_buffer(*type, &buff);

	return res;
}


int sdp_writeRegister(hid_device *dev, uint32_t addr, uint8_t format, uint32_t data)
{
	int rc;
	unsigned char b[BUF_SIZE]={0};

	/* Send write command */
	b[0] = 1;
	set_write_reg_cmd(b + 1, addr, format, data);
	if ((rc = hid_write(dev, b, CMD_SIZE)) < 0) {
		fprintf(stderr, "Failed to send write_register command (%d)\n", rc);
		return rc;
	}

	//Receive report 3
	if ((rc = hid_get_feature_report(dev, b, BUF_SIZE)) < 5) {
		fprintf(stderr, "Failed to receive HAB mode (n=%d)\n", rc);
		rc = -1;
		return rc;
	}

	if ((rc = hid_get_feature_report(dev, b, BUF_SIZE) < 0) || *(uint32_t*)(b + 1) != 0x128a8a12)
		fprintf(stderr, "Failed to receive complete status (status=%02x%02x%02x%02x)\n", b[1], b[2], b[3], b[4]);

	return rc;
}


int sdp_writeFile(hid_device *dev, uint32_t addr, void *data, size_t size)
{
	int n, rc;
	ssize_t offset = 0;
	unsigned char b[BUF_SIZE]={0};

	/* Send write command */
	b[0] = 1;
	set_write_file_cmd(b + 1, addr, size);
	if ((rc = hid_write(dev, b, CMD_SIZE)) < 0) {
		fprintf(stderr, "Failed to send write_file command (%d)\n", rc);
		return rc;
	}

	/* Send contents */
	b[0] = 2;
	while (offset < size) {
		n = (BUF_SIZE - 1 > size - offset) ? (size - offset) : (BUF_SIZE - 1);
		memcpy(b + 1, data + offset, n);
		offset += n;
		if((rc = hid_write(dev, b, n + 1)) < 0) {
			fprintf(stderr, "\nFailed to send image contents (%d)\n", rc);
			return rc;
		}
	}

	//Receive report 3
	if ((rc = hid_read(dev, b, BUF_SIZE)) < 5) {
		fprintf(stderr, "Failed to receive HAB mode (n=%d)\n", rc);
		rc = -1;
		return rc;
	}

	if ((rc = hid_read(dev, b, BUF_SIZE) < 0) || *(uint32_t*)(b + 1) != 0x88888888)
		fprintf(stderr, "Failed to receive complete status (status=%02x%02x%02x%02x)\n", b[1], b[2], b[3], b[4]);

	return rc;
}


int sdp_dcdWrite(hid_device *dev, uint32_t addr, void *data, size_t size)
{
	int n, rc;
	ssize_t offset = 0;
	unsigned char b[BUF_SIZE]={0};

	/* Send write command */
	b[0] = 1;
	set_dcd_write_cmd(b + 1, addr, size);
	if ((rc = hid_write(dev, b, CMD_SIZE)) < 0) {
		fprintf(stderr, "Failed to send dcd_write command (%d)\n", rc);
		return rc;
	}

	/* Send contents */
	b[0] = 2;
	while (offset < size) {
		n = (BUF_SIZE - 1 > size - offset) ? (size - offset) : (BUF_SIZE - 1);
		memcpy(b + 1, data + offset, n);
		offset += n;
		if((rc = hid_write(dev, b, n + 1)) < 0) {
			fprintf(stderr, "\nFailed to send image contents (%d)\n", rc);
			return rc;
		}
	}

	//Receive report 3
	if ((rc = hid_get_feature_report(dev, b, BUF_SIZE)) < 5) {
		fprintf(stderr, "Failed to receive HAB mode (n=%d)\n", rc);
		rc = -1;
		return rc;
	}

	if ((rc = hid_get_feature_report(dev, b, BUF_SIZE) < 0) || *(uint32_t*)(b + 1) != 0x12a8a812)
		fprintf(stderr, "Failed to receive complete status (status=%02x%02x%02x%02x)\n", b[1], b[2], b[3], b[4]);

	return rc;
}


int sdp_jmpAddr(hid_device *dev, uint32_t addr)
{
	int rc;
	unsigned char b[BUF_SIZE]={0};

	/* Send write command */
	b[0] = 1;
	set_jmp_cmd(b + 1, addr);
	if ((rc = hid_write(dev, b, CMD_SIZE)) < 0) {
		fprintf(stderr, "Failed to send jump_address command (%d)\n", rc);
		return rc;
	}

	/* Receive report 3 */
	if ((rc = hid_read(dev, b, BUF_SIZE)) < 5) {
		fprintf(stderr, "Failed to receive HAB mode (n=%d)\n", rc);
		rc = -1;
		return rc;
	}

	return rc;
}


int sdp_errStatus(hid_device *dev)
{
	unsigned char b[INTERRUPT_SIZE] = { 0 };
	int rc;
	b[0] = 1;
	set_status_cmd(b + 1);

	if((rc = hid_write(dev, b, CMD_SIZE)) < 0) {
		fprintf(stderr, "Failed to send status command (%d)\n", rc);
		return rc;
	}
	if((rc = hid_read(dev, b, INTERRUPT_SIZE)) < 5) {
		fprintf(stderr, "Failed to receive HAB mode (n=%d)\n", rc);
		return rc;
	}
	if((rc = hid_read(dev, b, INTERRUPT_SIZE)) < 0) {
		fprintf(stderr, "Failed to receive status (n=%d)\n", rc);
		return rc;
	}

	return rc;
}


int execute_line(char *line, size_t len, size_t lineno, hid_device **dev)
{
	int err = 0;
	char *tok = strtok(line, " ");
	size_t toklen = strlen(tok);

	if (tok[0] != '\n' && tok[0] != '#') { /* Skip empty lines and comments */
		if (tok[toklen - 1] == '\n') {
			tok[--toklen] = '\0';
		}

		fprintf(stderr, "Parsing %lu: '%s'\n", lineno, tok);

		if (!strcmp(tok, "WAIT")) {
			err = wait_cmd(dev);
		} else if(!strcmp(tok, "WRITE_FILE")) {
			err = write_file_cmd(*dev);
		} else if(!strcmp(tok, "WRITE_REGISTER")) {
			err = write_reg_cmd(*dev);
		} else if(!strcmp(tok, "JUMP_ADDRESS")) {
			err = jump_addr_cmd(*dev);
		} else if(!strcmp(tok, "ERROR_STATUS")) {
			err = err_status_cmd(*dev);
		} else if(!strcmp(tok, "DCD_WRITE")) {
		} else if(!strcmp(tok, "PROMPT")) {
		} else if(!strcmp(tok, "REBOOT")) {
		} else {
			fprintf(stderr, "Unrecognized token '%s' at line %lu\n", tok, lineno);
			err = -1;
		}
	}
	return err;
}


int main(int argc, char *argv[])
{
	FILE *script;
	int res = 0;
	size_t len = 1024, lineno = 0;
	char *buff = malloc(len);
	hid_device *dev = NULL;

	if (argc != 2) {
		usage(argv[0]);
		return -1;
	}

	hid_init();
	/* Interpret script */
	script = fopen(argv[1], "r");
	while (script != NULL && (res = getline(&buff, &len, script)) > 0) {
		if ((res = execute_line(buff, res, lineno++, &dev)) < 0) {
			fprintf(stderr, "Error at line %lu\n", lineno - 1);
			res = -1;
			break;
		}
	}

	hid_close(dev);
	if (script) {
		fclose(script);
	}
	hid_exit();
	free(buff);
	return res;
}

