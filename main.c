#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <scsi/scsi.h>
#include <scsi/sg.h>
#include <byteswap.h>

#define MAX_TRANSFER 60*1024

typedef struct it8951_inquiry {
	unsigned char dontcare[8];
	unsigned char vendor_id[8];
	unsigned char product_id[16];
	unsigned char product_ver[4];
} IT8951_inquiry;

typedef struct it8951_deviceinfo {
	unsigned int uiStandardCmdNo;
	unsigned int uiExtendedCmdNo;
	unsigned int uiSignature;
	unsigned int uiVersion;
	unsigned int width;
	unsigned int height;
	unsigned int update_buffer_addr;
	unsigned int image_buffer_addr;
	unsigned int temperature_segment;
	unsigned int ui_mode;
	unsigned int frame_count[8];
	unsigned int buffer_count;
	unsigned int reserved[9];
	void *command_table;
} IT8951_deviceinfo;

typedef struct it8951_display_area {
	u_int32_t address;
	u_int32_t wavemode;
	u_int32_t x;
	u_int32_t y;
	u_int32_t w;
	u_int32_t h;
	u_int32_t wait_ready;
} IT8951_display_area;

int debug = 0;
int clear = 0;

int memory_write(int fd, unsigned int addr, unsigned int length, char *data)
{
	unsigned char write_cmd[12] = {
		0xfe, 0x00,
		(addr >> 24) & 0xff,
		(addr >> 16) & 0xff,
		(addr >> 8) & 0xff,
		addr & 0xff,
		0x82,
		(length >> 8) & 0xff,
		length & 0xff,
		0x00, 0x00, 0x00
	};

	sg_io_hdr_t io_hdr;

	if (debug == 1) {
		for (u_int8_t i = 0; i < 12; i += 4) {
			printf("%02X %02X %02X %02X\n", write_cmd[i], write_cmd[i + 1],
				write_cmd[i + 2], write_cmd[i + 3]);
		}
		printf("\n");
	}

	memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
	io_hdr.interface_id = 'S';
	io_hdr.cmd_len = 12;
	io_hdr.dxfer_direction = SG_DXFER_TO_DEV;
	io_hdr.dxfer_len = length;
	io_hdr.dxferp = data;
	io_hdr.cmdp = write_cmd;
	io_hdr.timeout = 10000;

	if (ioctl(fd, SG_IO, &io_hdr) < 0) {
		perror("SG_IO memory write failed");
	}

	return 0;
}


int
display_area(int fd, u_int32_t addr, u_int32_t x, u_int32_t y, u_int32_t w, u_int32_t h, u_int32_t mode)
{
	unsigned char display_image_cmd[16] = {
		0xfe, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x94
	};

	printf("display area (0x%08x)\n", addr);
	IT8951_display_area area;
	memset(&area, 0, sizeof(IT8951_display_area));
	area.address = addr;
	area.x = x;
	area.y = y;
	area.w = w;
	area.h = h;
	area.wait_ready = 1;
	area.wavemode = mode;

	printf("Updating\n  Area X: 0x%08x\n  Area Y: 0x%08x\n  Area W: 0x%08x\n, Area H: 0x%08x\n", area.x, area.y, area.w, area.h);

	unsigned char *data_buffer = (unsigned char *) malloc(sizeof(IT8951_display_area));
	memcpy(data_buffer, &area, sizeof(IT8951_display_area));

	sg_io_hdr_t io_hdr;

	memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
	io_hdr.interface_id = 'S';
	io_hdr.cmd_len = 16;
	io_hdr.dxfer_direction = SG_DXFER_TO_DEV;
	io_hdr.dxfer_len = sizeof(IT8951_display_area);
	io_hdr.dxferp = data_buffer;
	io_hdr.cmdp = display_image_cmd;
	io_hdr.timeout = 50;

	if (ioctl(fd, SG_IO, &io_hdr) < 0) {
		perror("SG_IO display failed");
	}
	return 0;
}

int
pmic_set(int fd, int power, int vcom)
{


	unsigned char load_image_cmd[16] = {
		0xfe, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xa3,
		(vcom >> 8) & 0xff, vcom & 0xff,
		0x01,
		0x01, power & 0xff
	};

	sg_io_hdr_t io_hdr;

	memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
	io_hdr.interface_id = 'S';
	io_hdr.cmd_len = 16;
	io_hdr.dxfer_direction = SG_DXFER_TO_DEV;
	io_hdr.dxfer_len = 0;
	io_hdr.cmdp = load_image_cmd;
	io_hdr.timeout = 50;

	if (ioctl(fd, SG_IO, &io_hdr) < 0) {
		perror("SG_IO power set failed");
	}
	return 0;
}


void
update_region(const char *filename, int x, int y, int w, int h, int mode)
{
	int fd, to, res;
	fd = open(filename, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror("Could not open scsi device");
		exit(EXIT_FAILURE);
	}

	res = ioctl(fd, SCSI_IOCTL_GET_BUS_NUMBER, &to);
	if (res < 0) {
		fprintf(stderr, "%s is not a SCSI device\n", filename);
		exit(EXIT_FAILURE);
	}


	unsigned char inquiry_cmd[6] = {0x12, 0, 0, 0, 0, 0};
	unsigned char inquiry_result[96];
	unsigned char deviceinfo_cmd[16] = {
		0xfe, 0x00, // SCSI Customer command
		0x38, 0x39, 0x35, 0x31, // Chip signature
		0x80, 0x00, // Get System Info
		0x01, 0x00, 0x02, 0x00 // Version
	};
	unsigned char deviceinfo_result[112];


	sg_io_hdr_t io_hdr;

	memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
	io_hdr.interface_id = 'S';
	io_hdr.cmd_len = 6;
	io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
	io_hdr.dxfer_len = 96;
	io_hdr.dxferp = inquiry_result;
	io_hdr.cmdp = inquiry_cmd;
	io_hdr.timeout = 100;

	if (ioctl(fd, SG_IO, &io_hdr) < 0) {
		perror("SG_IO INQUIRY failed");
	}

	IT8951_inquiry *inquiry = (IT8951_inquiry *) inquiry_result;

	if (strncmp(inquiry->vendor_id, "Generic ", 8) != 0) {
		fprintf(stderr, "SCSI Vendor does not match\n");
		exit(EXIT_FAILURE);
	}
	if (strncmp(inquiry->product_id, "Storage RamDisc ", 8) != 0) {
		fprintf(stderr, "SCSI Product does not match\n");
		exit(EXIT_FAILURE);
	}
	if (strncmp(inquiry->product_ver, "1.00", 4) != 0) {
		fprintf(stderr, "SCSI Productver does not match\n");
		exit(EXIT_FAILURE);
	}

	if (debug == 1) {
		printf("Fetching device info\n");
	}

	memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
	io_hdr.interface_id = 'S';
	io_hdr.cmd_len = sizeof(deviceinfo_cmd);
	io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
	io_hdr.dxfer_len = 112;
	io_hdr.dxferp = deviceinfo_result;
	io_hdr.cmdp = deviceinfo_cmd;
	io_hdr.timeout = 10000;

	if (ioctl(fd, SG_IO, &io_hdr) < 0) {
		perror("SG_IO device info failed");
		exit(EXIT_FAILURE);
	}

	IT8951_deviceinfo *deviceinfo = (IT8951_deviceinfo *) deviceinfo_result;

	int width = __bswap_32(deviceinfo->width);
	int height = __bswap_32(deviceinfo->height);

	if (debug == 1) {
		printf("Found a %dx%d epaper display\n", width, height);
	}

	int addr = __bswap_32(deviceinfo->image_buffer_addr);

	if (debug == 1) {
		printf("Table version: 0x%08x\n", __bswap_32(deviceinfo->uiVersion));
		printf("Image buffer addr 0x%08x (0x%08x)\n", addr, __bswap_32(addr));
		printf("Update buffer addr 0x%08x\n", __bswap_32(deviceinfo->update_buffer_addr));
		printf("Temperature segment: %d\n", __bswap_32(deviceinfo->temperature_segment));
		printf("UI mode: %d\n", __bswap_32(deviceinfo->ui_mode));
		printf("Buffer count: 0x%08x\n", __bswap_32(deviceinfo->buffer_count));
	}


	int size = w * h;
	unsigned char *image = (unsigned char *) malloc(size);
	if (clear == 1) {
		printf("Filling buffer by dummy data (length %d)\n", size);
		memset(image, 0xFF, size);
	} else {
		size_t total_left = size;
		unsigned char *buffer_pointer = image;
		while (total_left > 0) {
			size_t current = read(STDIN_FILENO, buffer_pointer, total_left);
			if (current < 0) {
				perror("stdin read");
				exit(EXIT_FAILURE);
			} else if (current == 0) {
				fprintf(stderr, "stdin input is truncated\n");
				exit(EXIT_FAILURE);
			} else {
				total_left -= current;
				buffer_pointer += current;
			}
		}
	}

	unsigned int offset = 0;
	unsigned int startoffset = x + (width * y);
	unsigned int lines = MAX_TRANSFER / w;
	printf("Start offset: %d\n", startoffset);
	while (offset < size) {
		if ((offset / w) + lines > h) {
			lines = h - (offset / w);
			printf("Apply some h-offset)\n");
		}
		if (debug == 1) {
			printf("Sending %dx%d chunk to %d,%d (offset %d - lines %d)\n", w, lines, x, y + (offset / w), offset, lines);
		}
		memory_write(fd, addr+offset+startoffset, lines*w, &image[offset]);

		offset += lines * w;
	}
	if (debug == 1) {
		printf("Starting refresh\n");
	}
	display_area(fd, addr, x, y, w, h, mode);
}

void
print_usage(const char *name)
{
	fprintf(stderr, "Usage: %s [-m mode] [-dc] device x y w h\n", name);
	fprintf(stderr, "Options are:\n"
			"    -m: Refresh mode, 0=blank, 2=G16 (default), 4=A2\n"
			"    -d: Enable debug output\n"
			"    -c: Use a clean image instead of stdin\n"
			"    device: path to the disk device\n"
			"    x y: position of the image\n"
			"    w h: width and height of the image\n\n"
			"    Send the image to stdin as 8 bit grayscale\n");
	exit(EXIT_FAILURE);
}

int
main(int argc, char *argv[])
{
	int opt;
	int mode = 2;
	while ((opt = getopt(argc, argv, "m:dc")) != -1) {
		switch (opt) {
			case 'm':
				mode = strtol(optarg, NULL, 10);
				break;
			case 'd':
				debug = 1;
				break;
			case 'c':
				clear = 1;
				break;
			default:
				print_usage(argv[0]);
		}
	}

	if (argc - optind < 5) {
		print_usage(argv[0]);
	}
	int x = strtol(argv[optind + 1], NULL, 10);
	int y = strtol(argv[optind + 2], NULL, 10);
	int w = strtol(argv[optind + 3], NULL, 10);
	int h = strtol(argv[optind + 4], NULL, 10);

	update_region(argv[optind], x, y, w, h, mode);
	return 0;
}
