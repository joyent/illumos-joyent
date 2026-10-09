/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Carlos Neira.
 *
 * i386 LX auxv/vDSO regression probe.  Assemble with:
 *   as --32 -o auxv_sysinfo.o auxv_sysinfo.s
 *   ld -m elf_i386 -o auxv_sysinfo auxv_sysinfo.o
 *
 * Exit 10: AT_SYSINFO absent or zero
 * Exit 11: AT_SYSINFO_EHDR absent or zero
 * Exit 12: AT_SYSINFO != vDSO base + ELF e_entry
 * Exit 13: syscall through AT_SYSINFO failed
 * Exit 14: /proc/self/auxv disagrees with the initial stack
 */
	.text
	.globl _start
_start:
	xorl	%edi, %edi		/* AT_SYSINFO */
	xorl	%ebx, %ebx		/* AT_SYSINFO_EHDR */
	movl	(%esp), %ecx		/* argc */
	leal	8(%esp,%ecx,4), %esi	/* first environment pointer */
1:
	movl	(%esi), %eax
	addl	$4, %esi
	testl	%eax, %eax
	jne	1b
2:
	movl	(%esi), %eax		/* auxv type */
	testl	%eax, %eax
	je	3f
	cmpl	$32, %eax
	jne	4f
	movl	4(%esi), %edi
4:
	cmpl	$33, %eax
	jne	5f
	movl	4(%esi), %ebx
5:
	addl	$8, %esi
	jmp	2b
3:
	testl	%edi, %edi
	je	10f
	testl	%ebx, %ebx
	je	11f
	movl	24(%ebx), %edx		/* Elf32_Ehdr.e_entry */
	addl	%ebx, %edx
	cmpl	%edx, %edi
	jne	12f
	movl	$20, %eax		/* Linux i386 getpid */
	call	*%edi
	testl	%eax, %eax
	jle	13f
	subl	$512, %esp
	movl	$5, %eax		/* Linux i386 open */
	movl	$auxv_path, %ebx
	xorl	%ecx, %ecx
	int	$0x80
	testl	%eax, %eax
	js	14f
	movl	%eax, %ebx		/* auxv fd */
	movl	$3, %eax		/* Linux i386 read */
	movl	%esp, %ecx
	movl	$512, %edx
	int	$0x80
	testl	%eax, %eax
	jle	14f
	leal	(%esp,%eax), %esi	/* end of auxv data */
	movl	%esp, %edx
6:
	cmpl	%esi, %edx
	jae	14f
	movl	(%edx), %eax
	testl	%eax, %eax
	je	14f
	cmpl	$32, %eax
	je	7f
	addl	$8, %edx
	jmp	6b
7:
	cmpl	%edi, 4(%edx)
	jne	14f
	xorl	%ebx, %ebx
	jmp	99f
10:
	movl	$10, %ebx
	jmp	99f
11:
	movl	$11, %ebx
	jmp	99f
12:
	movl	$12, %ebx
	jmp	99f
13:
	movl	$13, %ebx
	jmp	99f
14:
	movl	$14, %ebx
99:
	movl	$1, %eax		/* Linux i386 exit */
	int	$0x80
	.section .rodata
auxv_path:
	.asciz	"/proc/self/auxv"
