.equ NR_rt_sigreturn, 139

.globl sigreturn

sigreturn:
	li      a7, NR_rt_sigreturn
	ecall

.type sigreturn,function
.size sigreturn,.-sigreturn