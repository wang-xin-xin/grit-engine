/* Copyright (c) David Cunningham and the Grit Game Engine project 2014
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "lua_util.h"
#include "LuaPtr.h"
#include "input_filter.h"
#include "gfx/gfx_hud.h"

typedef std::map<double, InputFilter *> IFMap;
static IFMap ifmap;

bool input_filter_has (double priority)
{
    IFMap::iterator i = ifmap.find(priority);
    return i != ifmap.end();
}

std::string input_filter_get_description (double priority)
{
    IFMap::iterator i = ifmap.find(priority);
    if (i == ifmap.end()) EXCEPT << "No InputFilter at priority " << priority << ENDL;
    return i->second->description;
}

struct Combo {
    bool ctrl, alt, shift;
    std::string prefix;
    Combo (void) { }
    Combo (bool ctrl, bool alt, bool shift, const std::string &prefix)
      : ctrl(ctrl), alt(alt), shift(shift), prefix(prefix) { }
};

Combo combos[8] = {
    Combo(true, true, true, "C-A-S-"),
    Combo(true, true, false, "C-A-"),
    Combo(true, false, true, "C-S-"),
    Combo(false, true, true, "A-S-"),
    Combo(true, false, false, "C-"),
    Combo(false, true, false, "A-"),
    Combo(false, false, true, "S-"),
    Combo(false, false, false, ""),
};

bool InputFilter::isBound (const std::string &ev)
{
    return buttonCallbacks.find(ev) != buttonCallbacks.end();
}

bool InputFilter::acceptButton (lua_State *L, const std::string &ev)
{
    bool ctrl = isButtonDown("Ctrl");
    bool alt = isButtonDown("Alt");
    bool shift = isButtonDown("Shift");
    return acceptButton(L, ev, ctrl, alt, shift);
}

bool InputFilter::acceptButton (lua_State *L, const std::string &ev, bool ctrl, bool alt, bool shift)
{
    ensureAlive();
    char kind = ev[0];
    std::string button = &ev[1];

    if (kind == '+') {

        std::vector<Combo> found;
        for (unsigned i=0 ; i<8 ; ++i) {
            const Combo &c = combos[i];
            // ignore prefixes that do not apply given current modifier status
            if (c.ctrl && !ctrl) continue;
            if (c.alt && !alt) continue;
            if (c.shift && !shift) continue;
            if (isBound(c.prefix+button)) found.push_back(c);
        }
        // if no bindings match, fall through
        if (found.size() == 0) return false;

        // should not already be down
        APP_ASSERT(!isButtonDown(button));

        const Combo &c = found[0];
        
        down[button] = c.prefix;

        const Callback &cb = buttonCallbacks.find(c.prefix+button)->second;
        
        const LuaPtr &func = cb.down;

        if (!func.isNil()) {
            triggerFunc(L, func);
        }

    } else if (kind == '-' || kind == '=') {

        ButtonStatus::iterator earlier = down.find(button);

        // silently ignore events if the button isn't down
        if (earlier == down.end()) return true;

        const std::string &prefix = earlier->second;

        // If it's down, there must be a callback for it
        const Callback &cb = buttonCallbacks.find(prefix+button)->second;

        if (kind == '-') {
            down.erase(button);
        }

        const LuaPtr &func = kind == '-' ? cb.up : cb.repeat;
        if (!func.isNil()) {
            triggerFunc(L, func);
        }

    }

    return false;
}

void InputFilter::triggerFunc (lua_State *L, const LuaPtr &func)
{
    ensureAlive();

    APP_ASSERT(!func.isNil());

    STACK_BASE;
    //stack is empty

    // error handler in case there is a problem during 
    // the callback
    push_cfunction(L, my_lua_error_handler);
    int error_handler = lua_gettop(L);

    //stack: err

    // get the function
    func.push(L);
    //stack: err,callback

    STACK_CHECK_N(2);

    // call (1 arg), pops function too
    int status = lua_pcall(L,0,0,error_handler);
    if (status) {
        STACK_CHECK_N(2);
        //stack: err,error
        // pop the error message since the error handler will
        // have already printed it out
        lua_pop(L,2);
        STACK_CHECK;
        CERR << "InputFilter \"" << description << "\" "
             << "raised an error on button callback, disabling." << std::endl;
        setEnabled(false);
        //stack is empty
    } else {
        //stack: err
        STACK_CHECK_N(1);
        lua_pop(L,1);
        //stack is empty
    }

    //stack is empty
    STACK_CHECK;
}

void InputFilter::triggerMouseMove (lua_State *L, const Vector2 &rel)
{
    ensureAlive();

    STACK_BASE;
    //stack is empty

    // error handler in case there is a problem during 
    // the callback
    push_cfunction(L, my_lua_error_handler);
    int error_handler = lua_gettop(L);

    //stack: err

    // get the function
    mouseMoveCallback.push(L);
    //stack: err,callback
    if (lua_isnil(L,-1)) {
        lua_pop(L,2);
        STACK_CHECK;
        CERR << "InputFilter \"" << description << "\" "
             << "has no mouseMoveCallback function, releasing mouse." << std::endl;
        setMouseCapture(false);
        return;
    }


    //stack: err,callback
    STACK_CHECK_N(2);

    push_v2(L, rel);
    //stack: err,callback,rel

    STACK_CHECK_N(3);

    // call (1 arg), pops function too
    int status = lua_pcall(L,1,0,error_handler);
    if (status) {
        STACK_CHECK_N(2);
        //stack: err,error
        // pop the error message since the error handler will
        // have already printed it out
        lua_pop(L,2);
        STACK_CHECK;
        CERR << "InputFilter \"" << description << "\" "
             << "raised an error on mouseMoveCallback, releasing mouse." << std::endl;
        setMouseCapture(false);
        //stack is empty
    } else {
        //stack: err
        STACK_CHECK_N(1);
        lua_pop(L,1);
        //stack is empty
    }

    //stack is empty
    STACK_CHECK;
}

void InputFilter::flushAll (lua_State *L)
{
    ensureAlive();
    // 'down' modified by acceptButton (and also potentially by callbacks).
    ButtonStatus d = down;
    for (ButtonStatus::iterator i=d.begin(),i_=d.end() ; i!=i_ ; ++i) {
        acceptButton(L, "-"+i->first);
    }
    APP_ASSERT(down.size() == 0);
}

void input_filter_trickle_button (lua_State *L, const std::string &b)
{
    // copy because callbacks can alter the map while we're iterating over it
    IFMap m = ifmap;
    bool captured = false;
    for (IFMap::const_iterator i=m.begin(),i_=m.end() ; i!=i_ ; ++i) {
        InputFilter *f = i->second;
        if (f->acceptButton(L, b)) return;
        if (f->getMouseCapture()) captured = true;
        if (f->getModal()) break;
    }
    if (!captured) gfx_hud_signal_button(L, b);
}

void input_filter_trickle_mouse_move (lua_State *L, const Vector2 &rel, const Vector2 &abs)
{
    // copy because callbacks can alter the map while we're iterating over it
    IFMap m = ifmap;
    for (IFMap::const_iterator i=m.begin(),i_=m.end() ; i!=i_ ; ++i) {
        InputFilter *f = i->second;
        if (f->getMouseCapture()) {
            f->triggerMouseMove(L, rel);
            return;
        }
        if (f->getModal()) break;
    }
    gfx_hud_signal_mouse_move(L, abs);
}

void input_filter_flush (lua_State *L)
{
    IFMap m = ifmap;
    for (IFMap::const_iterator i=m.begin(),i_=m.end() ; i!=i_ ; ++i) {
        InputFilter *f = i->second;
        f->flushAll(L);
    }
    gfx_hud_signal_flush(L);
}

void input_filter_shutdown (lua_State *L)
{
    IFMap m = ifmap;
    for (IFMap::const_iterator i=m.begin(),i_=m.end() ; i!=i_ ; ++i) {
        InputFilter *f = i->second;
        f->destroy(L);
    }
    APP_ASSERT(ifmap.size() == 0);
}


InputFilter::InputFilter (double priority, const std::string &desc)
  : modal(false), enabled(true), mouseCapture(false), priority(priority), destroyed(false), description(desc)
{
    IFMap::iterator i = ifmap.find(priority);
    if (i != ifmap.end()) {
        EXCEPT << "Cannot create InputFilter \"" << desc << "\" at priority already occupied by "
               << "\""<<i->second->description<<"\"" << std::endl;
    }
    ifmap[priority] = this;
}
    
InputFilter::~InputFilter (void)
{
    if (!destroyed) {
        CERR << "InputFilter \"" << description << "\" deleted without being destroyed first." << std::endl;
        mouseMoveCallback.leak();
        CallbackMap &m = buttonCallbacks;
        for (CallbackMap::iterator i=m.begin(),i_=m.end() ; i!=i_ ; ++i) {
            i->second.down.leak();
            i->second.up.leak();
            i->second.repeat.leak();
        }
        buttonCallbacks.clear();
        ifmap.erase(this->priority);
    }
}

void InputFilter::destroy (lua_State *L)
{
    ensureAlive();
    destroyed = true;
    mouseMoveCallback.setNil(L);
    CallbackMap &m = buttonCallbacks;
    for (CallbackMap::iterator i=m.begin(),i_=m.end() ; i!=i_ ; ++i) {
        i->second.down.setNil(L);
        i->second.up.setNil(L);
        i->second.repeat.setNil(L);
    }
    buttonCallbacks.clear();
    ifmap.erase(this->priority);
}

void InputFilter::bind (lua_State *L, const std::string &button)
{
    ensureAlive();

    Callback &c = buttonCallbacks[button];
    c.down.setNil(L);
    c.up.setNil(L);
    c.repeat.setNil(L);
    APP_ASSERT(lua_gettop(L) >= 3);
    // DOWN
    if (!lua_isnil(L, -3)) {
        if (!lua_isfunction(L, -3)) {
            EXCEPT << "Expected a function for down binding of " << button << ENDL;
        }
        c.down.setNoPop(L, -3);
    }
    // UP
    if (!lua_isnil(L, -2)) {
        if (!lua_isfunction(L, -2)) {
            EXCEPT << "Expected a function for up binding of " << button << ENDL;
        }
        c.up.setNoPop(L, -2);
    }
    // REPEAT
    if (!lua_isnil(L, -1)) {
        if (lua_isboolean(L, -1)) {
            if (lua_toboolean(L, -1)) {
                if (!lua_isnil(L, -3)) {
                    c.repeat.setNoPop(L, -3);
                }
            }
            // it's false, treat the same as nil
        } else {
            if (!lua_isfunction(L, -1)) {
                EXCEPT << "Expected a function or true for repeat binding of " << button << ENDL;
            }
            c.repeat.setNoPop(L, -1);
        }
    }
}

void InputFilter::unbind (lua_State *L, const std::string &button)
{
    ensureAlive();

    Callback &c = buttonCallbacks[button];
    c.down.setNil(L);
    c.up.setNil(L);
    c.repeat.setNil(L);
    buttonCallbacks.erase(button);
}

void InputFilter::setMouseMoveCallback (lua_State *L)
{
    ensureAlive();

    mouseMoveCallback.setNoPop(L);
}

void InputFilter::setEnabled (bool v)
{
    ensureAlive();

    enabled = v;
    // TODO: If disabling, release all local buttons.
    // If enabling, release all masked buttons below.
}

void InputFilter::setModal (bool v)
{
    ensureAlive();

    modal = v;
    // TODO: If enabling, release all buttons below.
}

void InputFilter::setMouseCapture (bool v)
{
    ensureAlive();

    mouseCapture = v;
    // TODO: If enabling, release all buttons in HUD.
}

bool InputFilter::isButtonDown (const std::string &b)
{
    ensureAlive();
    return down.find(b) != down.end();
}

/*
void binding_stack_set_state (lua_State *L, BindingLevel level, BindingState s)
{
    BindingStack::iterator i = binding_stack.find(level);
    if (i == binding_stack.end()) EXCEPT << "No binding stack at level: " << level << ENDL;
    BindingStack &stack = i->second;
    if (stack.modal == v) return;
    if (stack.
    if (stack.modal) {
        ButtonSet all_not_captured = stack.down - stack.captured;
        flush(L, stack, level, captured);
    } else {
        // send keydowns for keys down in binding tables below i 
    }
    stack.modal = v;
}
*/

