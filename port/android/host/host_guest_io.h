/* Guest Linux time and vector layouts. */
#ifndef HOST_GUEST_IO_H
#define HOST_GUEST_IO_H
struct guest_timespec
{
	int32_t seconds;
	int32_t nanoseconds;
};
struct guest_iovec
{
	uint32_t base;
	uint32_t length;
};
#endif
