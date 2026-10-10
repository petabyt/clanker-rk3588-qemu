#ifndef RK3588_HDMI_H
#define RK3588_HDMI_H

#define TYPE_RK3588_HDMI "rk3588-dw-hdmi-qp"
#define RK3588_HDMI(obj) OBJECT_CHECK(struct RkHdmiState, (obj), \
                                     TYPE_RK3588_HDMI)

#endif
