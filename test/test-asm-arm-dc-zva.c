/*
 * Copyright (C) 2026 Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

/*  Probe for the aarch64 DC zva cache maintenance instruction.
 *  DC instructions are available at EL0 on aarch64 Linux (the
 *  kernel maps them as trapped or direct depending on config),
 *  so the probe compiles and runs the instruction on a page the
 *  process owns. */
#if defined(__aarch64__)
int main(void)
{
	static char buf[64] __attribute__((aligned(64)));

	__asm__ __volatile__("dc zva, %0\n" : : "r"(buf) : "memory");

	return 0;
}
#else
#error not aarch64, no DC zva instruction
#endif
