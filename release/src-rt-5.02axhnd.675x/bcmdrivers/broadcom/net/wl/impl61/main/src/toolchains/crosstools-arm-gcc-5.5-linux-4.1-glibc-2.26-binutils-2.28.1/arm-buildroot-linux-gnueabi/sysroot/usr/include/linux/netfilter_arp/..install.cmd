cmd_/opt/toolchains/crosstools-arm-gcc-5.5-linux-4.1-glibc-2.26-binutils-2.28.1/arm-buildroot-linux-gnueabi/sysroot/usr/include/linux/netfilter_arp/.install := /bin/sh scripts/headers_install.sh /opt/toolchains/crosstools-arm-gcc-5.5-linux-4.1-glibc-2.26-binutils-2.28.1/arm-buildroot-linux-gnueabi/sysroot/usr/include/linux/netfilter_arp ./include/uapi/linux/netfilter_arp arp_tables.h arpt_mangle.h; /bin/sh scripts/headers_install.sh /opt/toolchains/crosstools-arm-gcc-5.5-linux-4.1-glibc-2.26-binutils-2.28.1/arm-buildroot-linux-gnueabi/sysroot/usr/include/linux/netfilter_arp ./include/linux/netfilter_arp ; /bin/sh scripts/headers_install.sh /opt/toolchains/crosstools-arm-gcc-5.5-linux-4.1-glibc-2.26-binutils-2.28.1/arm-buildroot-linux-gnueabi/sysroot/usr/include/linux/netfilter_arp ./include/generated/uapi/linux/netfilter_arp ; for F in ; do echo "\#include <asm-generic/$$F>" > /opt/toolchains/crosstools-arm-gcc-5.5-linux-4.1-glibc-2.26-binutils-2.28.1/arm-buildroot-linux-gnueabi/sysroot/usr/include/linux/netfilter_arp/$$F; done; touch /opt/toolchains/crosstools-arm-gcc-5.5-linux-4.1-glibc-2.26-binutils-2.28.1/arm-buildroot-linux-gnueabi/sysroot/usr/include/linux/netfilter_arp/.install