/* The host owns %fs; the guest runtime supplies its thread pointer. */
uintptr_t __guest_get_tp(void);

static inline uintptr_t __get_tp()
{
	return __guest_get_tp();
}

#define MC_PC gregs[REG_RIP]
#define CANARY_PAD
#define tls_mod_off_t unsigned long long
