/*
    This file is part of the Ansel project.
    Copyright (C) 2025 Aurélien PIERRE.
    
    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <gdk/gdkkeysyms.h>
#include <glib.h>


/**
 * @brief Find the numpad equivalent key of any given key.
 * Use this to define/handle alternative shortcuts.
 *
 * @param key_val
 * @return guint
 */
static inline guint dt_keys_numpad_alternatives(const guint key_val)
{
  guint alt_char = key_val;
  switch(key_val)
  {
    case GDK_KEY_0:
      alt_char = GDK_KEY_KP_0;
      break;
    case GDK_KEY_1:
      alt_char = GDK_KEY_KP_1;
      break;
    case GDK_KEY_2:
      alt_char = GDK_KEY_KP_2;
      break;
    case GDK_KEY_3:
      alt_char = GDK_KEY_KP_3;
      break;
    case GDK_KEY_4:
      alt_char = GDK_KEY_KP_4;
      break;
    case GDK_KEY_5:
      alt_char = GDK_KEY_KP_5;
      break;
    case GDK_KEY_6:
      alt_char = GDK_KEY_KP_6;
      break;
    case GDK_KEY_7:
      alt_char = GDK_KEY_KP_7;
      break;
    case GDK_KEY_8:
      alt_char = GDK_KEY_KP_8;
      break;
    case GDK_KEY_9:
      alt_char = GDK_KEY_KP_9;
      break;
    case GDK_KEY_Left:
      alt_char = GDK_KEY_KP_Left;
      break;
    case GDK_KEY_Right:
      alt_char = GDK_KEY_KP_Right;
      break;
    case GDK_KEY_Up:
      alt_char = GDK_KEY_KP_Up;
      break;
    case GDK_KEY_Down:
      alt_char = GDK_KEY_KP_Down;
      break;
    case GDK_KEY_Home:
      alt_char = GDK_KEY_KP_Home;
      break;
    case GDK_KEY_End:
      alt_char = GDK_KEY_KP_End;
      break;
    case GDK_KEY_Insert:
      alt_char = GDK_KEY_KP_Insert;
      break;
    case GDK_KEY_Return:
      alt_char = GDK_KEY_KP_Enter;
      break;
    case GDK_KEY_Page_Up:
      alt_char = GDK_KEY_KP_Page_Up;
      break;
    case GDK_KEY_Page_Down:
      alt_char = GDK_KEY_KP_Page_Down;
      break;
    case GDK_KEY_plus:
      alt_char = GDK_KEY_KP_Add;
      break;
    case GDK_KEY_minus:
      alt_char = GDK_KEY_KP_Subtract;
      break;
    default:
      break;
  }

  return alt_char;
}

/**
 * @brief Remap keypad keys to usual mainpad ones.
 *
 * @param key_val
 * @return guint
 */
static inline guint dt_keys_mainpad_alternatives(const guint key_val)
{
  guint alt_char = key_val;
  switch(key_val)
  {
    case GDK_KEY_KP_0:
      alt_char = GDK_KEY_0;
      break;
    case GDK_KEY_KP_1:
      alt_char = GDK_KEY_1;
      break;
    case GDK_KEY_KP_2:
      alt_char = GDK_KEY_2;
      break;
    case GDK_KEY_KP_3:
      alt_char = GDK_KEY_3;
      break;
    case GDK_KEY_KP_4:
      alt_char = GDK_KEY_4;
      break;
    case GDK_KEY_KP_5:
      alt_char = GDK_KEY_5;
      break;
    case GDK_KEY_KP_6:
      alt_char = GDK_KEY_6;
      break;
    case GDK_KEY_KP_7:
      alt_char = GDK_KEY_7;
      break;
    case GDK_KEY_KP_8:
      alt_char = GDK_KEY_8;
      break;
    case GDK_KEY_KP_9:
      alt_char = GDK_KEY_9;
      break;
    case GDK_KEY_KP_Left:
      alt_char = GDK_KEY_Left;
      break;
    case GDK_KEY_KP_Right:
      alt_char = GDK_KEY_Right;
      break;
    case GDK_KEY_KP_Up:
      alt_char = GDK_KEY_Up;
      break;
    case GDK_KEY_KP_Down:
      alt_char = GDK_KEY_Down;
      break;
    case GDK_KEY_KP_Home:
      alt_char = GDK_KEY_Home;
      break;
    case GDK_KEY_KP_End:
      alt_char = GDK_KEY_End;
      break;
    case GDK_KEY_KP_Insert:
      alt_char = GDK_KEY_Insert;
      break;
    case GDK_KEY_KP_Enter:
      alt_char = GDK_KEY_Return;
      break;
    case GDK_KEY_KP_Page_Up:
      alt_char = GDK_KEY_Page_Up;
      break;
    case GDK_KEY_KP_Page_Down:
      alt_char = GDK_KEY_Page_Down;
      break;
    case GDK_KEY_KP_Add:
      alt_char = GDK_KEY_plus;
      break;
    case GDK_KEY_KP_Subtract:
      alt_char = GDK_KEY_minus;
      break;
    default:
      break;
  }

  return alt_char;
}
