/* spi_whoami — read one register from an SPI sensor via spidev.
 *
 * Built to confirm the J106 onboard MPU-9250 IMU: WHO_AM_I (reg 0x75) = 0x71.
 * The IMU lives on /dev/spidev1.0 (Tegra186 SPI3 = spi@c260000) once the
 * tx2-j106-6csi/imu-mpu9250.dtsi pinmux is in place; `modprobe spidev` first.
 *
 *   gcc spi_whoami.c -o spi_whoami
 *   sudo ./spi_whoami /dev/spidev1.0 0x75     # -> 0x71
 *   sudo ./spi_whoami /dev/spidev1.0 0x3F     # accel Z high byte (~0x40 = +1g)
 *
 * args: [device] [reg] [mode]   defaults: /dev/spidev1.0 0x75 0
 * MPU-9250 register reads set the MSB (0x80); transfer is 2 bytes
 * [reg|0x80, 0x00] and the second received byte is the register value.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/spidev1.0";
	uint8_t reg  = argc > 2 ? (uint8_t)strtol(argv[2], 0, 0) : 0x75;
	uint8_t mode = argc > 3 ? (uint8_t)strtol(argv[3], 0, 0) : 0;
	int fd = open(dev, O_RDWR);
	if (fd < 0) { perror("open"); return 1; }

	uint8_t bits = 8;
	uint32_t speed = 1000000;
	ioctl(fd, SPI_IOC_WR_MODE, &mode);
	ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
	ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

	uint8_t tx[2] = { (uint8_t)(reg | 0x80), 0 }, rx[2] = { 0, 0 };
	struct spi_ioc_transfer tr;
	memset(&tr, 0, sizeof(tr));
	tr.tx_buf = (unsigned long)tx;
	tr.rx_buf = (unsigned long)rx;
	tr.len = 2;
	tr.speed_hz = speed;
	tr.bits_per_word = bits;
	if (ioctl(fd, SPI_IOC_MESSAGE(1), &tr) < 0) { perror("ioctl"); return 2; }

	printf("%s mode%d reg0x%02X -> 0x%02X\n", dev, mode, reg, rx[1]);
	close(fd);
	return 0;
}
