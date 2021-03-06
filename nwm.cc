/* This code is PUBLIC DOMAIN, and is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND. See the accompanying 
 * LICENSE file.
 */

#include <v8.h>
#include <node.h>
#include <ev.h>

#include <errno.h>
#include <locale.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>

#include <assert.h>   // I include this to test return values the lazy way
#include <unistd.h>   // So we got the profile for 10 seconds
#define NIL (0)       // A name for the void pointer
#define MAXWIN 512
#include "event_names.h"


using namespace node;
using namespace v8;

typedef struct {
  unsigned int mod;
  KeySym keysym;
} Key;

typedef struct Monitor Monitor;
typedef struct Client Client;
struct Client {
  int id;
  int x, y, width, height;
  Client *next;
  Client *snext;
  Monitor *mon;
  Window win;
};

struct Monitor {
  int id;
  int x, y, width, height;
  Client *clients;
//  Client *sel;
//  Client *stack;
  Monitor *next;
  Window barwin;
};

// make these classes of their own

enum callback_map { 
  onAdd, 
  onRemove,
  onRearrange,
  onMouseDown,
  onMouseDrag,
  onConfigureRequest,
  onKeyPress,
  onEnterNotify,
  onLast
};


class NodeWM: ObjectWrap
{
private:
  // X11
  Display *dpy;
  GC gc;
  Window wnd;
  Window root;
  Window selected;
  Monitor* monit;
  // screen dimensions
  int screen, screen_width, screen_height;
  // window id
  int next_index;
  // callback storage
  Persistent<Function>* callbacks[onLast];
  // grabbed keys
  Key keys[];
public:

  static Persistent<FunctionTemplate> s_ct;
  static void Init(Handle<Object> target) {
    HandleScope scope;
    // create a local FunctionTemplate
    Local<FunctionTemplate> t = FunctionTemplate::New(New);

    // initialize our template
    s_ct = Persistent<FunctionTemplate>::New(t);
    // set the field count
    s_ct->InstanceTemplate()->SetInternalFieldCount(1);
    // set the symbol for this function
    s_ct->SetClassName(String::NewSymbol("NodeWM"));

    // FUNCTIONS

    // callbacks
    NODE_SET_PROTOTYPE_METHOD(s_ct, "on", OnCallback);

    // API
    NODE_SET_PROTOTYPE_METHOD(s_ct, "moveWindow", MoveWindow);
    NODE_SET_PROTOTYPE_METHOD(s_ct, "resizeWindow", ResizeWindow);
    NODE_SET_PROTOTYPE_METHOD(s_ct, "focusWindow", FocusWindow);

    // Setting up
    NODE_SET_PROTOTYPE_METHOD(s_ct, "setup", Setup);
    NODE_SET_PROTOTYPE_METHOD(s_ct, "scan", Scan);
    NODE_SET_PROTOTYPE_METHOD(s_ct, "loop", Loop);

    // FINALLY: export the current function template
    target->Set(String::NewSymbol("NodeWM"),
                s_ct->GetFunction());
  }

  // C++ constructor
  NodeWM() :
    next_index(1),
    monit(NULL)
  {
  }

  ~NodeWM()
  {
  }

  // New method for v8
  static Handle<Value> New(const Arguments& args) {
    HandleScope scope;
    NodeWM* hw = new NodeWM();
    for(int i = 0; i < onLast; i++) {
      hw->callbacks[i] = NULL;
    }
    // use ObjectWrap.Wrap to store hw in this
    hw->Wrap(args.This());
    // return this
    return args.This();
  }

  // EVENTS

  static Handle<Value> OnCallback(const Arguments& args) {
    HandleScope scope;
    // extract from args.this
    NodeWM* hw = ObjectWrap::Unwrap<NodeWM>(args.This());

    v8::Local<v8::String> map[onLast+1] = {
        v8::String::New("add"),
        v8::String::New("remove"), 
        v8::String::New("rearrange"),
        v8::String::New("buttonPress"),
        v8::String::New("configureRequest"),
        v8::String::New("keyPress"),
        v8::String::New("enterNotify"),
      };

    v8::Local<v8::String> value  = Local<v8::String>::Cast(args[0]);
    int selected = -1;
    for(int i = 0; i < onLast; i++) {
      if( strcmp(*v8::String::AsciiValue(value),  *v8::String::AsciiValue(map[i])) == 0 ) {
        selected = i;
        break;        
      }
    }
    if(selected == -1) {
      return Undefined();
    }
    // store function
    hw->callbacks[selected] = cb_persist(args[1]);

    return Undefined();
  }

  void Emit(callback_map event, int argc, Handle<Value> argv[]) {
    TryCatch try_catch;
    if(this->callbacks[event] != NULL) {
      Handle<Function> *callback = cb_unwrap(this->callbacks[event]);
      (*callback)->Call(Context::GetCurrent()->Global(), argc, argv);                
      if (try_catch.HasCaught()) {
        FatalException(try_catch);
      }
    }
  }

  // Client management

  static void attach(Client *c) {
    c->next = c->mon->clients;
    c->mon->clients = c;
  }

  static void detach(Client *c) {
    Client **tc;
    for(tc = &c->mon->clients; *tc && *tc != c; tc = &(*tc)->next);
    *tc = c->next;    
  }

  static Client* createClient(Window win, Monitor* monitor, int id, int x, int y, int width, int height) {
    Client *c;
    if(!(c = (Client *)calloc(1, sizeof(Client)))) {
      fprintf( stderr, "fatal: could not malloc() %lu bytes\n", sizeof(Client));
      exit( -1 );      
    }
    c->win = win;
    c->mon = monitor;
    c->id = id;
    c->x = x;
    c->y = y;
    c->width = width;
    c->height = height;
    return c;
  }

  static Monitor* createMonitor(void) {
    Monitor *m;
    if(!(m = (Monitor *)calloc(1, sizeof(Monitor)))) {
      fprintf( stderr, "fatal: could not malloc() %lu bytes\n", sizeof(Monitor));
      exit( -1 );            
    }
   fprintf( stderr, "Create monitor\n");
    return m;    
  }

  static Client* getByWindow(NodeWM* hw, Window win) {
    Client *c;
//    Monitor *m;
    for(c = hw->monit->clients; c; c = c->next)
      if(c->win == win)
        return c;
    return NULL;
  }

  static Client* getById(NodeWM* hw, int id) {
    Client *c;
    for(c = hw->monit->clients; c; c = c->next)
      if(c->id == id)
        return c;
    return NULL;  
  }

  static void updateGeometry(NodeWM* hw) {
    if(!hw->monit)
      hw->monit = createMonitor();
    if(hw->monit->width != hw->screen_width || hw->monit->height != hw->screen_width){
      hw->monit->width = hw->screen_width;
      hw->monit->height = hw->screen_height;
    }
    return;
  }

  /**
   * Prepare the window object and call the Node.js callback.
   */
  static void EmitAdd(NodeWM* hw, Window win, XWindowAttributes *wa) {
    TryCatch try_catch;
    // onManage receives a window object
    Local<Value> argv[1];
    // temporarily store window to hw->wnd
    Client* c = createClient(win, hw->monit, hw->next_index, wa->x, wa->y, wa->height, wa->width);
    attach(c);
    argv[0] = NodeWM::makeWindow(hw->next_index, wa->x, wa->y, wa->height, wa->width, wa->border_width);
    hw->next_index++;

    // call the callback in Node.js, passing the window object...
    hw->Emit(onAdd, 1, argv);
      
    // configure the window
    XConfigureEvent ce;

    ce.type = ConfigureNotify;
    ce.display = hw->dpy;
    ce.event = win;
    ce.window = win;
    ce.x = wa->x;
    ce.y = wa->y;
    ce.width = wa->width;
    ce.height = wa->height;
    ce.border_width = wa->border_width;
    ce.above = None;
    ce.override_redirect = False;

    (void) fprintf( stderr, "manage: x=%d y=%d width=%d height=%d \n", ce.x, ce.y, ce.width, ce.height);

    XSendEvent(hw->dpy, win, False, StructureNotifyMask, (XEvent *)&ce);

  
    // subscribe to window events
    XSelectInput(hw->dpy, win, EnterWindowMask|FocusChangeMask|PropertyChangeMask|StructureNotifyMask);
    GrabButtons(hw->dpy, win, False);

    // move and (finally) map the window
    XMoveResizeWindow(hw->dpy, win, ce.x, ce.y, ce.width, ce.height);    
    XMapWindow(hw->dpy, win);

    // emit a rearrange
    hw->Emit(onRearrange, 0, 0);
  }

  static Handle<Value> ResizeWindow(const Arguments& args) {
    HandleScope scope;
    NodeWM* hw = ObjectWrap::Unwrap<NodeWM>(args.This());

    int id = args[0]->IntegerValue();
    int width = args[1]->IntegerValue();
    int height = args[2]->IntegerValue();

    Client* c = getById(hw, id);
    if(c && c->win) {
      fprintf( stderr, "ResizeWindow: id=%d width=%d height=%d \n", id, width, height);    
      XResizeWindow(hw->dpy, c->win, width, height);    
      XFlush(hw->dpy);
    }
    return Undefined();
  } 

  static Handle<Value> MoveWindow(const Arguments& args) {
    HandleScope scope;
    NodeWM* hw = ObjectWrap::Unwrap<NodeWM>(args.This());

    int id = args[0]->IntegerValue();
    int x = args[1]->IntegerValue();
    int y = args[2]->IntegerValue();

    Client* c = getById(hw, id);
    if(c && c->win) {
      fprintf( stderr, "MoveWindow: id=%d x=%d y=%d \n", id, x, y);    
      XMoveWindow(hw->dpy, c->win, x, y);    
      XFlush(hw->dpy);
    }
    return Undefined();
  }

  static Handle<Value> FocusWindow(const Arguments& args) {
    HandleScope scope;
    NodeWM* hw = ObjectWrap::Unwrap<NodeWM>(args.This());

    int id = args[0]->IntegerValue();
    RealFocus(hw, id);
    return Undefined();
  }

  static void RealFocus(NodeWM* hw, int id) {
    Window win;
    Client* c = getById(hw, id);
    if(c && c->win) {
      win = c->win;
    } else {
      win = hw->root;
    }
    fprintf( stderr, "FocusWindow: id=%d\n", id);    
    // do not focus on the same window... it'll cause a flurry of events...
    if(hw->selected && hw->selected != win) {
      GrabButtons(hw->dpy, win, True);
      XSetInputFocus(hw->dpy, win, RevertToPointerRoot, CurrentTime);    
      Atom atom = XInternAtom(hw->dpy, "WM_TAKE_FOCUS", False);
      SendEvent(hw, win, atom);
      XFlush(hw->dpy);      
      hw->selected = win;
    }
  }

  static Bool SendEvent(NodeWM* hw, Window wnd, Atom proto) {
    int n;
    Atom *protocols;
    Bool exists = False;
    XEvent ev;

    if(XGetWMProtocols(hw->dpy, wnd, &protocols, &n)) {
      while(!exists && n--)
        exists = protocols[n] == proto;
      XFree(protocols);
    }
    if(exists) {
      ev.type = ClientMessage;
      ev.xclient.window = wnd;
      Atom atom = XInternAtom(hw->dpy, "WM_PROTOCOLS", False);
      ev.xclient.message_type = atom;
      ev.xclient.format = 32;
      ev.xclient.data.l[0] = proto;
      ev.xclient.data.l[1] = CurrentTime;
      XSendEvent(hw->dpy, wnd, False, NoEventMask, &ev);
    }
    return exists;    
  }

  /**
   * If focused, then we only grab the modifier keys.
   * Otherwise, we grab all buttons..
   */
  static void GrabButtons(Display* dpy, Window wnd, Bool focused) {
    XUngrabButton(dpy, AnyButton, AnyModifier, wnd);
    if(focused) {
      XGrabButton(dpy, Button3,
                        Mod4Mask,
                        wnd, False, (ButtonPressMask|ButtonReleaseMask),
                        GrabModeAsync, GrabModeSync, None, None);
    } else {
      XGrabButton(dpy, AnyButton, AnyModifier, wnd, False,
                  (ButtonPressMask|ButtonReleaseMask), GrabModeAsync, GrabModeSync, None, None);
    }
  }

  /**
   * Set the keys to grab
   */
  static Handle<Value> GrabKeys(const Arguments& args) {
    HandleScope scope;
    int i;
    Key k;
    v8::Handle<v8::Value> val;
    // extract from args.this
    NodeWM* hw = ObjectWrap::Unwrap<NodeWM>(args.This());
    v8::Local<v8::Object> obj = Local<v8::Object>::Cast(args[0]);

    for(i = 0; i < obj.length; i++) {
      val = obj->Get(String::NewSymbol("key"));    
      k.keysym = val->IntegerValue();
      val = obj->Get(String::NewSymbol("modifier"));
      k.mod = val->IntegerValue();
      hw->keys[i] = k;
    }
    return Undefined();
  }

  static void GrabKeys(Display* dpy, Window root) {
    XUngrabKey(dpy, AnyKey, AnyModifier, root);

    for(int i = 0; i < hw->keys.length; i++) {
      XGrabKey(dpy, XKeysymToKeycode(dpy, keys[i].keysym), keys[i].mod, root, True, GrabModeAsync, GrabModeAsync);
    }

    /*
    // Valid keymap bits are: ShiftMask, LockMask, ControlMask, Mod1Mask, Mod2Mask, Mod3Mask, Mod4Mask, and Mod5Mask
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_1), Mod4Mask|ControlMask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_2), Mod4Mask|ControlMask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_3), Mod4Mask|ControlMask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_4), Mod4Mask|ControlMask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_5), Mod4Mask|ControlMask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_6), Mod4Mask|ControlMask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_7), Mod4Mask|ControlMask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_8), Mod4Mask|ControlMask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_9), Mod4Mask|ControlMask, root, True, GrabModeAsync, GrabModeAsync);

    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_1), Mod4Mask|ControlMask|ShiftMask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_2), Mod4Mask|ControlMask|ShiftMask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_3), Mod4Mask|ControlMask|ShiftMask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_4), Mod4Mask|ControlMask|ShiftMask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_5), Mod4Mask|ControlMask|ShiftMask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_6), Mod4Mask|ControlMask|ShiftMask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_7), Mod4Mask|ControlMask|ShiftMask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_8), Mod4Mask|ControlMask|ShiftMask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_9), Mod4Mask|ControlMask|ShiftMask, root, True, GrabModeAsync, GrabModeAsync);

    XGrabKey(dpy, XKeysymToKeycode(dpy, XK_Return), Mod4Mask|ControlMask, root, True, GrabModeAsync, GrabModeAsync);
    */
  }

  static Local<Object> makeWindow(int id, int x, int y, int height, int width, int border_width) {
    // window object to return
    Local<Object> result = Object::New();

    // read and set the window geometry
    result->Set(String::NewSymbol("id"), Integer::New(id));
    result->Set(String::NewSymbol("x"), Integer::New(x));
    result->Set(String::NewSymbol("y"), Integer::New(y));
    result->Set(String::NewSymbol("height"), Integer::New(height));
    result->Set(String::NewSymbol("width"), Integer::New(width));
    result->Set(String::NewSymbol("border_width"), Integer::New(border_width));
    // read and set the monitor (not important)
//    result->Set(String::NewSymbol("monitor"), Integer::New(hw->selected_monitor));
    return result;
  }

  static void EmitButtonPress(NodeWM* hw, XEvent *e) {
    XButtonPressedEvent *ev = &e->xbutton;
    Local<Value> argv[1];

    fprintf(stderr, "EmitButtonPress\n");

    // fetch window: ev->window --> to window id
    // fetch root_x,root_y
    Client* c = getByWindow(hw, ev->window);
    if(c) {
      int id = c->id;
      argv[0] = NodeWM::makeButtonPress(id, ev->x, ev->y, ev->button, ev->state);
      fprintf(stderr, "makeButtonPress\n");
      // call the callback in Node.js, passing the window object...
      hw->Emit(onMouseDown, 1, argv);
      fprintf(stderr, "Call cbButtonPress\n");
    }
  }

  Bool getrootptr(int *x, int *y) {
    int di;
    unsigned int dui;
    Window dummy;

    return XQueryPointer(dpy, root, &dummy, &dummy, x, y, &di, &di, &dui);
  }

  static Handle<Value> GrabMouseRelease(const Arguments& args) {
    XEvent ev;
    int x, y;
    Local<Value> argv[1];

    if(XGrabPointer(dpy, root, False, 
      ButtonPressMask|ButtonReleaseMask|PointerMotionMask, GrabModeAsync, 
      GrabModeAsync, None, XCreateFontCursor(dpy, XC_fleur), CurrentTime) != GrabSuccess) {
      return Undefined();
    }
    if(!getrootptr(&x, &y)) {
      return Undefined();
    }
    do{
      XMaskEvent(dpy, ButtonPressMask|ButtonReleaseMask|PointerMotionMask|ExposureMask|SubstructureRedirectMask, &ev);
      switch(ev.type) {
        case ConfigureRequest:
          // handle normally
          break;
        case Expose:
          // handle normally
          break;
        case MapRequest:
          // handle normally
          break;
        case MotionNotify:
          {          
            argv[0] = NodeWM::makeMouseDrag(x, y, ev.xmotion.x, ev.xmotion.y, ev.state);
            hw->Emit(onMouseDrag, 1, argv);
          }
          break;
      }
    } while(ev.type != ButtonRelease);

    XUngrabPointer(dpy, CurrentTime);
  }


  static Local<Object> makeMouseDrag(int x, int y, int movex, int movey, unsigned int state) {
    // window object to return
    Local<Object> result = Object::New();

    // read and set the window geometry
    result->Set(String::NewSymbol("x"), Integer::New(x));
    result->Set(String::NewSymbol("y"), Integer::New(y));
    result->Set(String::NewSymbol("move_x"), Integer::New(movex));
    result->Set(String::NewSymbol("move_y"), Integer::New(movey));
    result->Set(String::NewSymbol("state"), Integer::New(state));
    return result;
  }


  static Local<Object> makeButtonPress(int id, int x, int y, unsigned int button, unsigned int state) {
    // window object to return
    Local<Object> result = Object::New();

    // read and set the window geometry
    result->Set(String::NewSymbol("id"), Integer::New(id));
    result->Set(String::NewSymbol("x"), Integer::New(x));
    result->Set(String::NewSymbol("y"), Integer::New(y));
    result->Set(String::NewSymbol("button"), Integer::New(button));
    result->Set(String::NewSymbol("state"), Integer::New(state));
    return result;
  }

  static void EmitKeyPress(NodeWM* hw, XEvent *e) {
    KeySym keysym;
    XKeyEvent *ev;

    ev = &e->xkey;
    keysym = XKeycodeToKeysym(hw->dpy, (KeyCode)ev->keycode, 0);
    Local<Value> argv[1];
    argv[0] = NodeWM::makeKeyPress(ev->x, ev->y, ev->keycode, keysym, ev->state);
    // call the callback in Node.js, passing the window object...
    hw->Emit(onKeyPress, 1, argv);
  }

  static Local<Object> makeKeyPress(int x, int y, unsigned int keycode, KeySym keysym, unsigned int mod) {
    // window object to return
    Local<Object> result = Object::New();
    // read and set the window geometry
    result->Set(String::NewSymbol("x"), Integer::New(x));
    result->Set(String::NewSymbol("y"), Integer::New(y));
    result->Set(String::NewSymbol("keysym"), Integer::New(keysym));
    result->Set(String::NewSymbol("keycode"), Integer::New(keycode));
    result->Set(String::NewSymbol("mod"), Integer::New(mod));
    return result;
  }

  static void EmitEnterNotify(NodeWM* hw, XEvent *e) {
    XCrossingEvent *ev = &e->xcrossing;
    // onManage receives a window object
    Local<Value> argv[1];

    fprintf(stderr, "EmitEnterNotify\n");

    Client* c = getByWindow(hw, ev->window);
    if(c) {
      int id = c->id;
      argv[0] = NodeWM::makeEvent(id);
      // call the callback in Node.js, passing the window object...
      hw->Emit(onEnterNotify, 1, argv);
    }
  }

  static void EmitFocusIn(NodeWM* hw, XEvent *e) {
    XFocusChangeEvent *ev = &e->xfocus;
    Client* c = getByWindow(hw, ev->window);
    if(c) {
      int id = c->id;
      RealFocus(hw, id);
    }
  }

  static void EmitUnmapNotify(NodeWM* hw, XEvent *e) {
    Client *c;
    XUnmapEvent *ev = &e->xunmap;
    if((c = getByWindow(hw, ev->window)))
      EmitRemove(hw, c, False);
  }

  static void EmitDestroyNotify(NodeWM* hw, XEvent *e) {
    Client *c;
    XDestroyWindowEvent *ev = &e->xdestroywindow;

    if((c = getByWindow(hw, ev->window)))
      EmitRemove(hw, c, True);    
  }

  static void EmitRemove(NodeWM* hw, Client *c, Bool destroyed) {
//    Monitor *m = c->mon;
//    XWindowChanges wc;
    fprintf( stderr, "EmitRemove\n");
    int id = c->id;
    // emit a remove
    Local<Value> argv[1];
    argv[0] = Integer::New(id);
    hw->Emit(onRemove, 1, argv);
    detach(c);
    if(!destroyed) {
      XGrabServer(hw->dpy);
      XUngrabButton(hw->dpy, AnyButton, AnyModifier, c->win);
      XSync(hw->dpy, False);
      XUngrabServer(hw->dpy);
    }
    free(c);
    RealFocus(hw, -1);
    hw->Emit(onRearrange, 0, 0);
  }

  static Local<Object> makeEvent(int id) {
    // window object to return
    Local<Object> result = Object::New();
    result->Set(String::NewSymbol("id"), Integer::New(id));
    return result;
  }


  static Handle<Value> Setup(const Arguments& args) {
    HandleScope scope;
    // extract from args.this
    NodeWM* hw = ObjectWrap::Unwrap<NodeWM>(args.This());

    // initialize resources
    // atoms

    // open the display
    if ( ( hw->dpy = XOpenDisplay(NIL) ) == NULL ) {
      (void) fprintf( stderr, "cannot connect to X server %s\n", XDisplayName(NULL));
      exit( -1 );
    }
    // set error handler
    XSetErrorHandler(xerror);
    XSync(hw->dpy, False);

    // take the default screen
    hw->screen = DefaultScreen(hw->dpy);
    // get the root window
    hw->root = RootWindow(hw->dpy, hw->screen);

    // get screen geometry
    hw->screen_width = DisplayWidth(hw->dpy, hw->screen);
    hw->screen_height = DisplayHeight(hw->dpy, hw->screen);
    // update monitor geometry (and create hw->monitor)
    updateGeometry(hw);

    XSetWindowAttributes wa;
    // subscribe to root window events e.g. SubstructureRedirectMask
    wa.event_mask = SubstructureRedirectMask|SubstructureNotifyMask|ButtonPressMask
                    |EnterWindowMask|LeaveWindowMask|StructureNotifyMask
                    |PropertyChangeMask;
    XSelectInput(hw->dpy, hw->root, wa.event_mask);

    GrabKeys(hw->dpy, hw->root);

    Local<Object> result = Object::New();
    result->Set(String::NewSymbol("width"), Integer::New(hw->screen_width));
    result->Set(String::NewSymbol("height"), Integer::New(hw->screen_height));
    return scope.Close(result);
  }



  /**
   * Scan and layout current windows
   */
  static Handle<Value> Scan(const Arguments& args) {
    HandleScope scope;
    // extract from args.this
    NodeWM* hw = ObjectWrap::Unwrap<NodeWM>(args.This());

    unsigned int i, num;
    Window d1, d2, *wins = NULL;
    XWindowAttributes wa;
    // XQueryTree() function returns the root ID, the parent window ID, a pointer to 
    // the list of children windows (NULL when there are no children), and 
    // the number of children in the list for the specified window. 
    if(XQueryTree(hw->dpy, hw->root, &d1, &d2, &wins, &num)) {
      for(i = 0; i < num; i++) {
        // if we can't read the window attributes, 
        // or the window is a popup (transient or override_redirect), skip it
        if(!XGetWindowAttributes(hw->dpy, wins[i], &wa)
        || wa.override_redirect || XGetTransientForHint(hw->dpy, wins[i], &d1)) {
          continue;          
        }
        // visible or minimized window ("Iconic state")
        if(wa.map_state == IsViewable )//|| getstate(wins[i]) == IconicState)
          NodeWM::EmitAdd(hw, wins[i], &wa);
      }
      for(i = 0; i < num; i++) { /* now the transients */
        if(!XGetWindowAttributes(hw->dpy, wins[i], &wa))
          continue;
        if(XGetTransientForHint(hw->dpy, wins[i], &d1)
        && (wa.map_state == IsViewable )) //|| getstate(wins[i]) == IconicState))
          NodeWM::EmitAdd(hw, wins[i], &wa);
      }
      if(wins) {
        // To free a non-NULL children list when it is no longer needed, use XFree()
        XFree(wins);
      }
    }

    Local<String> result = String::New("Scan done");
    return scope.Close(result);
  }

  static Handle<Value> Loop(const Arguments& args) {
    HandleScope scope;
    // extract from args.this
    NodeWM* hw = ObjectWrap::Unwrap<NodeWM>(args.This());

    // use ev_io

    // initiliaze and start 
    ev_io_init(&hw->watcher, EIO_RealLoop, XConnectionNumber(hw->dpy), EV_READ);
    hw->watcher.data = hw;
    ev_io_start(EV_DEFAULT_ &hw->watcher);

//    hw->Ref();

    return Undefined();
  }

  static void EIO_RealLoop(EV_P_ struct ev_io* watcher, int revents) {

    NodeWM* hw = static_cast<NodeWM*>(watcher->data);    
    
    XEvent event;
    // main event loop 
    XSync(hw->dpy, False);
    while(XPending(hw->dpy)) {
      XNextEvent(hw->dpy, &event);
      fprintf(stderr, "got event %s (%d).\n", event_names[event.type], event.type);      
      // handle event internally --> calls Node if necessary 
      switch (event.type) {
        case ButtonPress:
          {
            fprintf(stderr, "EmitButtonPress\n");
            NodeWM::EmitButtonPress(hw, &event);
          }
          break;
        case ConfigureRequest:
            break;
        case ConfigureNotify:
            break;
        case DestroyNotify:
            NodeWM::EmitDestroyNotify(hw, &event);        
            break;
        case EnterNotify:
            NodeWM::EmitEnterNotify(hw, &event);
            break;
        case Expose:
            break;
        case FocusIn:
         //   NodeWM::EmitFocusIn(hw, &event);
            break;
        case KeyPress:
          {
            fprintf(stderr, "EmitKeyPress\n");
            NodeWM::EmitKeyPress(hw, &event);
          }
            break;
        case MappingNotify:
            break;
        case MapRequest:
          {
            // read the window attrs, then add it to the managed windows...
            XWindowAttributes wa;
            XMapRequestEvent *ev = &event.xmaprequest;
            if(!XGetWindowAttributes(hw->dpy, ev->window, &wa)) {
              fprintf(stderr, "XGetWindowAttributes failed\n");              
              return;
            }
            if(wa.override_redirect)
              return;
            fprintf(stderr, "MapRequest\n");
            Client* c = NodeWM::getByWindow(hw, ev->window);
            if(c == NULL) {
              fprintf(stderr, "Emit manage!\n");
              // dwm actually does this only once per window (e.g. for unknown windows only...)
              // that's because otherwise you'll cause a hang when you map a mapped window again...
              NodeWM::EmitAdd(hw, ev->window, &wa);
            } else {
              fprintf(stderr, "Window is known\n");              
            }
          }
            break;
        case PropertyNotify:
            break;
        case UnmapNotify:
            NodeWM::EmitUnmapNotify(hw, &event);
            break;
        default:
            break;
      }
    }
    return;
  }

  static int xerror(Display *dpy, XErrorEvent *ee) {
    if(ee->error_code == BadWindow
    || (ee->request_code == X_SetInputFocus && ee->error_code == BadMatch)
    || (ee->request_code == X_PolyText8 && ee->error_code == BadDrawable)
    || (ee->request_code == X_PolyFillRectangle && ee->error_code == BadDrawable)
    || (ee->request_code == X_PolySegment && ee->error_code == BadDrawable)
    || (ee->request_code == X_ConfigureWindow && ee->error_code == BadMatch)
    || (ee->request_code == X_GrabButton && ee->error_code == BadAccess)
    || (ee->request_code == X_GrabKey && ee->error_code == BadAccess)
    || (ee->request_code == X_CopyArea && ee->error_code == BadDrawable))
      return 0;
    fprintf(stderr, "dwm: fatal error: request code=%d, error code=%d\n",
        ee->request_code, ee->error_code);
//    return xerrorxlib(dpy, ee); /* may call exit */    
    exit(-1);
    return 0;
  }

/*
  static Handle<Value> GetWindow(const Arguments& args) {
    HandleScope scope;
    
    // how to return a new object
//    Local<Object> result = Object::New();
//    result->Set(String::NewSymbol("test"), Integer::New(123));
//    return scope.Close(result);

    // how to return a new function
    Local<Object> result = Object::New();
    result->Set(String::NewSymbol("test"), 
      v8::FunctionTemplate::New(WindowObject::Test)->GetFunction());

    return scope.Close(result);
  }
*/
  ev_io watcher;
};

Persistent<FunctionTemplate> NodeWM::s_ct;

extern "C" {
  // target for export
  static void init (Handle<Object> target)
  {
    NodeWM::Init(target);
  }

  // macro to export
  NODE_MODULE(nwm, init);
}
