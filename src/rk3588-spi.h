#ifndef RK3588_SPI_H
#define RK3588_SPI_H

#define TYPE_RK3588_SPI "rk3588-spi"
#define RK3588_SPI(obj) OBJECT_CHECK(struct RkSpiState, (obj), \
                                     TYPE_RK3588_SPI)

#endif