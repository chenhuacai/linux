/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Loongson Technology Corporation Limited
 * Authors: Hanlu Li <lihanlu@loongson.cn>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef _TRANS_MIPS_SYSCALLS_H
#define _TRANS_MIPS_SYSCALLS_H

#define M_ENOMSG		35
#define M_EIDRM			36
#define M_ECHRNG		37
#define M_EL2NSYNC		38
#define M_EL3HLT		39
#define M_EL3RST		40
#define M_ELNRNG		41
#define M_EUNATCH		42
#define M_ENOCSI		43
#define M_EL2HLT		44
#define M_EDEADLK		45
#define M_ENOLCK		46
#define M_EBADE			50
#define M_EBADR			51
#define M_EXFULL		52
#define M_ENOANO		53
#define M_EBADRQC		54
#define M_EBADSLT		55
#define M_EBFONT		59
#define M_ENOSTR		60
#define M_ENODATA		61
#define M_ETIME			62
#define M_ENOSR			63
#define M_ENONET		64
#define M_ENOPKG		65
#define M_EREMOTE		66
#define M_ENOLINK		67
#define M_EADV			68
#define M_ESRMNT		69
#define M_ECOMM			70
#define M_EPROTO		71
#define M_EDOTDOT		73
#define M_EMULTIHOP		74
#define M_EBADMSG		77
#define M_ENAMETOOLONG		78
#define M_EOVERFLOW		79
#define M_ENOTUNIQ		80
#define M_EBADFD		81
#define M_EREMCHG		82
#define M_ELIBACC		83
#define M_ELIBBAD		84
#define M_ELIBSCN		85
#define M_ELIBMAX		86
#define M_ELIBEXEC		87
#define M_EILSEQ		88
#define M_ENOSYS		89
#define M_ELOOP			90
#define M_ERESTART		91
#define M_ESTRPIPE		92
#define M_ENOTEMPTY		93
#define M_EUSERS		94
#define M_ENOTSOCK		95
#define M_EDESTADDRREQ		96
#define M_EMSGSIZE		97
#define M_EPROTOTYPE		98
#define M_ENOPROTOOPT		99
#define M_EPROTONOSUPPORT	120
#define M_ESOCKTNOSUPPORT	121
#define M_EOPNOTSUPP		122
#define M_EPFNOSUPPORT		123
#define M_EAFNOSUPPORT		124
#define M_EADDRINUSE		125
#define M_EADDRNOTAVAIL		126
#define M_ENETDOWN		127
#define M_ENETUNREACH		128
#define M_ENETRESET		129
#define M_ECONNABORTED		130
#define M_ECONNRESET		131
#define M_ENOBUFS		132
#define M_EISCONN		133
#define M_ENOTCONN		134
#define M_EUCLEAN		135
#define M_ENOTNAM		137
#define M_ENAVAIL		138
#define M_EISNAM		139
#define M_EREMOTEIO		140
#define M_ESHUTDOWN		143
#define M_ETOOMANYREFS		144
#define M_ETIMEDOUT		145
#define M_ECONNREFUSED		146
#define M_EHOSTDOWN		147
#define M_EHOSTUNREACH		148
#define M_EALREADY		149
#define M_EINPROGRESS		150
#define M_ESTALE		151
#define M_ECANCELED		158
#define M_ENOMEDIUM		159
#define M_EMEDIUMTYPE		160
#define M_ENOKEY		161
#define M_EKEYEXPIRED		162
#define M_EKEYREVOKED		163
#define M_EKEYREJECTED		164
#define M_EOWNERDEAD		165
#define M_ENOTRECOVERABLE	166
#define M_ERFKILL		167
#define M_EHWPOISON		168
#define M_EDQUOT		1133
#endif /* _TRANS_MIPS_SYSCALLS_H  */
