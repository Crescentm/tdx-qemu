#ifndef QEMU_I386_TDX_VMCALL_SERVICE_H
#define QEMU_I386_TDX_VMCALL_SERVICE_H

#define TDG_VP_VMCALL_SERVICE_SUCCESS                   0
#define TDG_VP_VMCALL_SERVICE_DEVICE_ERROR              1
#define TDG_VP_VMCALL_SERVICE_TIME_OUT                  2
#define TDG_VP_VMCALL_SERVICE_RSP_BUF_TOO_SMALL         3
#define TDG_VP_VMCALL_SERVICE_BAD_CMD_BUF_SIZE          4
#define TDG_VP_VMCALL_SERVICE_BAD_RSP_BUF_SIZE          5
#define TDG_VP_VMCALL_SERVICE_BUSY                      6
#define TDG_VP_VMCALL_SERVICE_INVALID_OPERAND           7
#define TDG_VP_VMCALL_SERVICE_OUT_OF_RESOURCE           8
#define TDG_VP_VMCALL_SERVICE_NOT_SUPPORT               0xFFFFFFFE

#endif
