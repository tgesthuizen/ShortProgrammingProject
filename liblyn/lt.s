	.thumb
	.syntax unified
	.section ".text", "ax"
	.global "<"
	.type "<", %function
	.func "<", "<"
"<":
	movs	r3, #1
	cmp	r0, r1
	blt	.L2
	eors	r3, r3
.L2:
        bx      lr
	.size "<", .-"<"
