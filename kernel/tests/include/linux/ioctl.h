#ifndef TBSTREAM_TEST_LINUX_IOCTL_H
#define TBSTREAM_TEST_LINUX_IOCTL_H

/* asm-generic ioctl encoding used by the x86_64 deployment hosts. */
#define _IOC_NRBITS	8
#define _IOC_TYPEBITS	8
#define _IOC_SIZEBITS	14
#define _IOC_DIRBITS	2

#define _IOC_NRSHIFT	0
#define _IOC_TYPESHIFT	(_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT	(_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT	(_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE	0U
#define _IOC_WRITE	1U
#define _IOC_READ	2U

#define _IOC(dir, type, nr, size) \
	(((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | \
	 ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))
#define _IOC_TYPECHECK(type)	(sizeof(type))

#define _IO(type, nr)		_IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, arg)	_IOC(_IOC_READ, (type), (nr), \
				     _IOC_TYPECHECK(arg))
#define _IOW(type, nr, arg)	_IOC(_IOC_WRITE, (type), (nr), \
				     _IOC_TYPECHECK(arg))
#define _IOWR(type, nr, arg)	_IOC(_IOC_READ | _IOC_WRITE, (type), (nr), \
				     _IOC_TYPECHECK(arg))

#define _IOC_DIR(cmd)	(((cmd) >> _IOC_DIRSHIFT) & ((1U << _IOC_DIRBITS) - 1))
#define _IOC_TYPE(cmd)	(((cmd) >> _IOC_TYPESHIFT) & \
			 ((1U << _IOC_TYPEBITS) - 1))
#define _IOC_NR(cmd)	(((cmd) >> _IOC_NRSHIFT) & ((1U << _IOC_NRBITS) - 1))
#define _IOC_SIZE(cmd)	(((cmd) >> _IOC_SIZESHIFT) & \
			 ((1U << _IOC_SIZEBITS) - 1))

#endif /* TBSTREAM_TEST_LINUX_IOCTL_H */
