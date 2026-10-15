#ifndef RK3588_EDP_H
#define RK3588_EDP_H

#define TYPE_RK3588_EDP "rk3588-edp"
#define RK3588_EDP(obj) OBJECT_CHECK(struct RkEdpState, (obj), \
                                     TYPE_RK3588_EDP)

#endif