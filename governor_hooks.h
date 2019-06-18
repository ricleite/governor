/*
 * Copyright (C) 2019 Ricardo Leite
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef __GOVERNOR_HOOKS_H_
#define __GOVERNOR_HOOKS_H_

// program start/exit hooks
void initializer();
void finalizer();
// thread hooks that must be called by Governor code
// if sub_hook() is called by a thread, unsub_hook() will
//  be called when that thread exits
void sub_hook();
void unsub_hook(void* argptr);

#endif // __GOVERNOR_HOOKS_H_