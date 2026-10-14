#ifndef RK3588_I2C_H
#define RK3588_I2C_H

#define TYPE_RK3588_I2C "rk3588-i2c"
#define RK3588_I2C(obj) OBJECT_CHECK(struct RkI2cState, (obj), \
                                     TYPE_RK3588_I2C)

#endif
