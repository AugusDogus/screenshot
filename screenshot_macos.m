#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

static const CGFloat OVERLAY_ALPHA = 0.4;
static const CGFloat HANDLE_SIZE = 4.0;
static const CGFloat BORDER_WIDTH = 1.0;
static const CGFloat MIN_SEL_SIZE = 2.0;

typedef NS_ENUM(NSInteger, HANDLE_ID) {
  HT_NONE = -1,
  HT_TL = 0,
  HT_T,
  HT_TR,
  HT_L,
  HT_R,
  HT_BL,
  HT_B,
  HT_BR
};

// --- OverlayView - handles drawing and mouse interaction ---
@interface OverlayView : NSView
@property (nonatomic, strong) NSImage *capturedImage;
@property (nonatomic, assign) BOOL haveSel;
@property (nonatomic, assign) NSRect sel;
@property (nonatomic, assign) BOOL selecting;
@property (nonatomic, assign) BOOL resizing;
@property (nonatomic, assign) BOOL moving;
@property (nonatomic, assign) NSPoint dragStart;
@property (nonatomic, assign) NSPoint moveOffset;
@property (nonatomic, assign) HANDLE_ID activeHandle;
@property (nonatomic, assign) NSRect resizeAnchor;
@end

@implementation OverlayView

- (instancetype)initWithFrame:(NSRect)frame {
  self = [super initWithFrame:frame];
  if (self) {
    _haveSel = NO;
    _selecting = NO;
    _resizing = NO;
    _moving = NO;
    _activeHandle = HT_NONE;
  }
  return self;
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView { return YES; }

- (NSRect)normalizeSel {
  NSRect s = self.sel;
  if (s.size.width < 0) {
    s.origin.x += s.size.width;
    s.size.width = -s.size.width;
  }
  if (s.size.height < 0) {
    s.origin.y += s.size.height;
    s.size.height = -s.size.height;
  }
  return s;
}

// --- Handle helpers & swapping ---
- (NSArray<NSValue *> *)getHandleCenters:(NSRect)r {
  CGFloat cx = NSMidX(r), cy = NSMidY(r);
  return @[
    [NSValue valueWithPoint:NSMakePoint(NSMinX(r), NSMaxY(r))],  // TL
    [NSValue valueWithPoint:NSMakePoint(cx, NSMaxY(r))],         // T
    [NSValue valueWithPoint:NSMakePoint(NSMaxX(r), NSMaxY(r))],  // TR
    [NSValue valueWithPoint:NSMakePoint(NSMinX(r), cy)],         // L
    [NSValue valueWithPoint:NSMakePoint(NSMaxX(r), cy)],         // R
    [NSValue valueWithPoint:NSMakePoint(NSMinX(r), NSMinY(r))],  // BL
    [NSValue valueWithPoint:NSMakePoint(cx, NSMinY(r))],         // B
    [NSValue valueWithPoint:NSMakePoint(NSMaxX(r), NSMinY(r))]   // BR
  ];
}

- (NSRect)handleRect:(NSPoint)c {
  return NSMakeRect(c.x - HANDLE_SIZE, c.y - HANDLE_SIZE,
                    HANDLE_SIZE * 2, HANDLE_SIZE * 2);
}

- (HANDLE_ID)hitTest:(NSPoint)pt inRect:(NSRect)r {
  if (r.size.width < 1 || r.size.height < 1) return HT_NONE;

  NSArray<NSValue *> *centers = [self getHandleCenters:r];
  for (NSInteger i = 0; i < 8; i++) {
    NSRect hr = [self handleRect:centers[i].pointValue];
    if (NSPointInRect(pt, hr))
      return (HANDLE_ID)i;
  }

  const CGFloat EDGE = HANDLE_SIZE + 2;
  NSRect top = NSMakeRect(NSMinX(r) + EDGE, NSMaxY(r) - EDGE,
                          r.size.width - EDGE * 2, EDGE * 2);
  NSRect bot = NSMakeRect(NSMinX(r) + EDGE, NSMinY(r) - EDGE,
                          r.size.width - EDGE * 2, EDGE * 2);
  NSRect left = NSMakeRect(NSMinX(r) - EDGE, NSMinY(r) + EDGE,
                           EDGE * 2, r.size.height - EDGE * 2);
  NSRect right = NSMakeRect(NSMaxX(r) - EDGE, NSMinY(r) + EDGE,
                            EDGE * 2, r.size.height - EDGE * 2);

  if (NSPointInRect(pt, top)) return HT_T;
  if (NSPointInRect(pt, bot)) return HT_B;
  if (NSPointInRect(pt, left)) return HT_L;
  if (NSPointInRect(pt, right)) return HT_R;

  return HT_NONE;
}

- (NSCursor *)cursorForHandle:(HANDLE_ID)h {
  switch (h) {
  case HT_T:
  case HT_B:
    return [NSCursor resizeUpDownCursor];
  case HT_L:
  case HT_R:
    return [NSCursor resizeLeftRightCursor];
  case HT_TL:
  case HT_BR:
    return [NSCursor crosshairCursor];
  case HT_TR:
  case HT_BL:
    return [NSCursor crosshairCursor];
  default:
    return [NSCursor crosshairCursor];
  }
}

- (HANDLE_ID)swapH:(HANDLE_ID)h {
  switch (h) {
  case HT_L: return HT_R;
  case HT_R: return HT_L;
  case HT_TL: return HT_TR;
  case HT_TR: return HT_TL;
  case HT_BL: return HT_BR;
  case HT_BR: return HT_BL;
  default: return h;
  }
}

- (HANDLE_ID)swapV:(HANDLE_ID)h {
  switch (h) {
  case HT_T: return HT_B;
  case HT_B: return HT_T;
  case HT_TL: return HT_BL;
  case HT_BL: return HT_TL;
  case HT_TR: return HT_BR;
  case HT_BR: return HT_TR;
  default: return h;
  }
}

// --- Drawing helpers ---
- (void)drawRect:(NSRect)dirtyRect {
  if (!self.capturedImage) return;

  NSRect bounds = self.bounds;

  // Draw captured image
  [self.capturedImage drawInRect:bounds];

  // Draw dark overlay
  [[NSColor colorWithWhite:0.0 alpha:OVERLAY_ALPHA] setFill];
  NSRectFillUsingOperation(bounds, NSCompositingOperationSourceOver);

  if (self.haveSel) {
    NSRect s = [self normalizeSel];

    // Draw clear selection area (show original image)
    NSGraphicsContext *ctx = [NSGraphicsContext currentContext];
    [ctx saveGraphicsState];
    NSRectClip(s);
    [self.capturedImage drawInRect:bounds];
    [ctx restoreGraphicsState];

    // Draw selection border (white dotted line)
    [[NSColor whiteColor] setStroke];
    NSBezierPath *border = [NSBezierPath bezierPathWithRect:s];
    CGFloat pattern[] = {4, 4};
    [border setLineDash:pattern count:2 phase:0];
    [border setLineWidth:BORDER_WIDTH];
    [border stroke];

    [self drawHandles:s];
    [self drawDimsLabel:s];
  }
}

- (void)drawHandles:(NSRect)r {
  NSArray<NSValue *> *centers = [self getHandleCenters:r];

  for (NSValue *val in centers) {
    NSPoint center = val.pointValue;
    NSRect hr = [self handleRect:center];

    [[NSColor blackColor] setFill];
    [[NSColor whiteColor] setStroke];
    NSBezierPath *path = [NSBezierPath bezierPathWithRect:hr];
    [path setLineWidth:1.0];
    [path fill];
    [path stroke];
  }
}

- (void)drawDimsLabel:(NSRect)s {
  NSInteger w = (NSInteger)s.size.width;
  NSInteger h = (NSInteger)s.size.height;
  NSString *dims = [NSString stringWithFormat:@"%ldx%ld", (long)w, (long)h];

  NSDictionary *attrs = @{
    NSFontAttributeName: [NSFont systemFontOfSize:12],
    NSForegroundColorAttributeName: [NSColor colorWithWhite:0.94 alpha:1.0]
  };

  NSSize textSize = [dims sizeWithAttributes:attrs];
  const CGFloat padX = 6, padY = 3, outside = HANDLE_SIZE + 6, inside = 6;
  CGFloat boxW = textSize.width + padX * 2;
  CGFloat boxH = textSize.height + padY * 2;

  NSRect box;
  NSRect bounds = self.bounds;

  // Try left of selection
  box = NSMakeRect(NSMinX(s) - outside - boxW, NSMaxY(s) - boxH, boxW, boxH);
  if (box.origin.x >= 0) goto draw_label;

  // Try above selection
  box = NSMakeRect(NSMinX(s), NSMaxY(s) + outside, boxW, boxH);
  if (NSMaxY(box) <= bounds.size.height) goto draw_label;

  // Inside selection (top-left corner)
  box = NSMakeRect(NSMinX(s) + inside, NSMaxY(s) - boxH - inside, boxW, boxH);

draw_label:
  // Clamp to bounds
  if (box.origin.x < 0) box.origin.x = 0;
  if (box.origin.y < 0) box.origin.y = 0;
  if (NSMaxX(box) > bounds.size.width) box.origin.x = bounds.size.width - boxW;
  if (NSMaxY(box) > bounds.size.height) box.origin.y = bounds.size.height - boxH;

  [[NSColor blackColor] setFill];
  NSRectFill(box);

  NSPoint textPoint = NSMakePoint(box.origin.x + padX, box.origin.y + padY);
  [dims drawAtPoint:textPoint withAttributes:attrs];
}

- (void)mouseDown:(NSEvent *)event {
  NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];

  if (self.haveSel) {
    NSRect s = [self normalizeSel];
    HANDLE_ID h = [self hitTest:p inRect:s];

    if (h != HT_NONE) {
      self.resizing = YES;
      self.selecting = self.moving = NO;
      self.activeHandle = h;
      self.resizeAnchor = s;
      return;
    }

    if (NSPointInRect(p, s)) {
      self.moving = YES;
      self.selecting = self.resizing = NO;
      self.moveOffset = NSMakePoint(p.x - s.origin.x, p.y - s.origin.y);
      return;
    }
  }

  // new selection
  self.selecting = YES;
  self.resizing = self.moving = NO;
  self.haveSel = YES;
  self.dragStart = p;
  self.sel = NSMakeRect(p.x, p.y, 0, 0);
  [self setNeedsDisplay:YES];
}

- (void)mouseDragged:(NSEvent *)event {
  NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  NSRect bounds = self.bounds;

  if (self.selecting) {
    CGFloat x = MIN(self.dragStart.x, p.x);
    CGFloat y = MIN(self.dragStart.y, p.y);
    CGFloat w = fabs(p.x - self.dragStart.x);
    CGFloat h = fabs(p.y - self.dragStart.y);
    self.sel = NSMakeRect(x, y, w, h);
    [self setNeedsDisplay:YES];
  } else if (self.resizing) {
    [self resizeRobust:p];
    [self setNeedsDisplay:YES];
  } else if (self.moving) {
    NSRect s = [self normalizeSel];
    CGFloat newX = p.x - self.moveOffset.x;
    CGFloat newY = p.y - self.moveOffset.y;

    // clamp to bounds
    if (newX < 0) newX = 0;
    if (newY < 0) newY = 0;
    if (newX + s.size.width > bounds.size.width)
      newX = bounds.size.width - s.size.width;
    if (newY + s.size.height > bounds.size.height)
      newY = bounds.size.height - s.size.height;

    self.sel = NSMakeRect(newX, newY, s.size.width, s.size.height);
    [self setNeedsDisplay:YES];
  }
}

// robust resize
- (void)resizeRobust:(NSPoint)p {
  HANDLE_ID h = self.activeHandle;
  NSRect anchor = self.resizeAnchor;
  CGFloat L = NSMinX(anchor), R = NSMaxX(anchor);
  CGFloat B = NSMinY(anchor), T = NSMaxY(anchor);

  if (h == HT_R || h == HT_TR || h == HT_BR) {
    if (p.x < L) {
      R = L;
      h = [self swapH:h];
      anchor = NSMakeRect(R, NSMinY(anchor), 0, anchor.size.height);
      self.resizeAnchor = anchor;
    } else {
      R = MAX(p.x, L + MIN_SEL_SIZE);
    }
  } else if (h == HT_L || h == HT_TL || h == HT_BL) {
    if (p.x > R) {
      L = R;
      h = [self swapH:h];
      anchor = NSMakeRect(L, NSMinY(anchor), 0, anchor.size.height);
      self.resizeAnchor = anchor;
    } else {
      L = MIN(p.x, R - MIN_SEL_SIZE);
    }
  }

  if (h == HT_T || h == HT_TL || h == HT_TR) {
    if (p.y < B) {
      T = B;
      h = [self swapV:h];
      anchor = NSMakeRect(NSMinX(anchor), T, anchor.size.width, 0);
      self.resizeAnchor = anchor;
    } else {
      T = MAX(p.y, B + MIN_SEL_SIZE);
    }
  } else if (h == HT_B || h == HT_BL || h == HT_BR) {
    if (p.y > T) {
      B = T;
      h = [self swapV:h];
      anchor = NSMakeRect(NSMinX(anchor), B, anchor.size.width, 0);
      self.resizeAnchor = anchor;
    } else {
      B = MIN(p.y, T - MIN_SEL_SIZE);
    }
  }

  self.activeHandle = h;
  self.sel = NSMakeRect(L, B, R - L, T - B);

  // clamp to bounds
  NSRect bounds = self.bounds;
  NSRect s = self.sel;
  if (s.origin.x < 0) s.origin.x = 0;
  if (s.origin.y < 0) s.origin.y = 0;
  if (NSMaxX(s) > bounds.size.width) s.size.width = bounds.size.width - s.origin.x;
  if (NSMaxY(s) > bounds.size.height) s.size.height = bounds.size.height - s.origin.y;
  self.sel = s;
}

- (void)mouseUp:(NSEvent *)event {
  self.selecting = self.resizing = self.moving = NO;
}

- (void)mouseMoved:(NSEvent *)event {
  NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];

  if (self.haveSel && !self.selecting && !self.resizing && !self.moving) {
    NSRect s = [self normalizeSel];
    HANDLE_ID h = [self hitTest:p inRect:s];

    if (h != HT_NONE) {
      [[self cursorForHandle:h] set];
    } else if (NSPointInRect(p, s)) {
      [[NSCursor openHandCursor] set];
    } else {
      [[NSCursor crosshairCursor] set];
    }
  } else {
    [[NSCursor crosshairCursor] set];
  }
}

- (void)updateTrackingAreas {
  [super updateTrackingAreas];

  for (NSTrackingArea *area in self.trackingAreas) {
    [self removeTrackingArea:area];
  }

  NSTrackingArea *trackingArea = [[NSTrackingArea alloc]
      initWithRect:self.bounds
           options:(NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect)
             owner:self
          userInfo:nil];
  [self addTrackingArea:trackingArea];
}

- (void)keyDown:(NSEvent *)event {
  if (event.keyCode == kVK_Escape) {
    [self.window close];
  } else if (event.keyCode == kVK_Return ||
             ((event.modifierFlags & NSEventModifierFlagCommand) && event.keyCode == kVK_ANSI_C)) {
    [self copySelectionToClipboard];
    [self.window close];
  } else {
    [super keyDown:event];
  }
}

- (void)rightMouseDown:(NSEvent *)event {
  [self.window close];
}

- (BOOL)copySelectionToClipboard {
  if (!self.haveSel) return NO;

  NSRect s = [self normalizeSel];
  if (s.size.width <= 0 || s.size.height <= 0) return NO;

  NSImage *selImage = [[NSImage alloc] initWithSize:s.size];
  [selImage lockFocus];
  [self.capturedImage drawInRect:NSMakeRect(0, 0, s.size.width, s.size.height)
                        fromRect:s
                       operation:NSCompositingOperationCopy
                        fraction:1.0];
  [selImage unlockFocus];

  NSPasteboard *pb = [NSPasteboard generalPasteboard];
  [pb clearContents];
  [pb writeObjects:@[selImage]];

  return YES;
}

@end

// --- OverlayWindow - borderless fullscreen window ---
@interface OverlayWindow : NSWindow
@end

@implementation OverlayWindow

- (instancetype)initWithContentRect:(NSRect)contentRect
                          styleMask:(NSWindowStyleMask)style
                            backing:(NSBackingStoreType)backingStoreType
                              defer:(BOOL)flag {
  self = [super initWithContentRect:contentRect
                          styleMask:NSWindowStyleMaskBorderless
                            backing:NSBackingStoreBuffered
                              defer:NO];
  if (self) {
    self.level = NSStatusWindowLevel;
    self.backgroundColor = [NSColor clearColor];
    self.opaque = NO;
    self.hasShadow = NO;
    self.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                              NSWindowCollectionBehaviorFullScreenAuxiliary;
  }
  return self;
}

- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }

@end

// --- AppDelegate - manages menu bar icon and hotkey ---
@interface AppDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic, strong) NSStatusItem *statusItem;
@property (nonatomic, strong) OverlayWindow *overlayWindow;
@property (nonatomic, assign) EventHotKeyRef hotKeyRef;
- (void)launchOverlay;
- (void)captureAndShowOverlay;
- (void)showOverlayWithImage:(NSImage *)capturedImage frame:(NSRect)frame;
@end

static AppDelegate *g_appDelegate = nil;

OSStatus HotKeyHandler(EventHandlerCallRef nextHandler, EventRef event, void *userData) {
  if (g_appDelegate) {
    dispatch_async(dispatch_get_main_queue(), ^{
      [g_appDelegate launchOverlay];
    });
  }
  return noErr;
}

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
  g_appDelegate = self;

  // Create status bar item
  self.statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
  self.statusItem.button.image = [NSImage imageWithSystemSymbolName:@"camera.viewfinder"
                                               accessibilityDescription:@"Screenshot"];
  if (!self.statusItem.button.image) {
    self.statusItem.button.title = @"📷";
  }

  // Create menu
  NSMenu *menu = [[NSMenu alloc] init];

  NSMenuItem *captureItem = [[NSMenuItem alloc] initWithTitle:@"Take Screenshot"
                                                       action:@selector(launchOverlay)
                                                keyEquivalent:@""];
  captureItem.target = self;
  [menu addItem:captureItem];

  [menu addItem:[NSMenuItem separatorItem]];

  NSMenuItem *quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit"
                                                    action:@selector(terminate:)
                                             keyEquivalent:@"q"];
  [menu addItem:quitItem];

  self.statusItem.menu = menu;

  [self registerHotKey];
}

- (void)registerHotKey {
  EventHotKeyID hotKeyID;
  hotKeyID.signature = 'SCRN';
  hotKeyID.id = 1;

  EventTypeSpec eventType;
  eventType.eventClass = kEventClassKeyboard;
  eventType.eventKind = kEventHotKeyPressed;

  InstallApplicationEventHandler(&HotKeyHandler, 1, &eventType, NULL, NULL);

  // Register Cmd+Shift+4 (like macOS native screenshot)
  RegisterEventHotKey(21, cmdKey + shiftKey, hotKeyID,
                      GetApplicationEventTarget(), 0, &_hotKeyRef);
}

- (void)launchOverlay {
  if (self.overlayWindow) {
    [self.overlayWindow close];
    self.overlayWindow = nil;
  }

  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.1 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
    [self captureAndShowOverlay];
  });
}

- (void)captureAndShowOverlay {
  if (@available(macOS 12.3, *)) {
    [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent *content, NSError *error) {
      if (error || !content) {
        dispatch_async(dispatch_get_main_queue(), ^{
          NSAlert *alert = [[NSAlert alloc] init];
          alert.messageText = @"Screen Recording Permission Required";
          alert.informativeText = @"Please grant Screen Recording permission in System Settings > Privacy & Security > Screen Recording, then restart the app.";
          [alert runModal];
        });
        return;
      }

      SCDisplay *mainDisplay = content.displays.firstObject;
      if (!mainDisplay) {
        dispatch_async(dispatch_get_main_queue(), ^{
          NSAlert *alert = [[NSAlert alloc] init];
          alert.messageText = @"No Display Found";
          alert.informativeText = @"Could not find a display to capture.";
          [alert runModal];
        });
        return;
      }

      SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:mainDisplay excludingWindows:@[]];
      SCStreamConfiguration *config = [[SCStreamConfiguration alloc] init];
      config.width = mainDisplay.width * 2;  // Retina
      config.height = mainDisplay.height * 2;
      config.showsCursor = NO;
      config.pixelFormat = kCVPixelFormatType_32BGRA;

      [SCScreenshotManager captureImageWithFilter:filter
                                    configuration:config
                                completionHandler:^(CGImageRef cgImage, NSError *captureError) {
        if (captureError || !cgImage) {
          dispatch_async(dispatch_get_main_queue(), ^{
            NSAlert *alert = [[NSAlert alloc] init];
            alert.messageText = @"Capture Failed";
            alert.informativeText = captureError.localizedDescription ?: @"Unknown error";
            [alert runModal];
          });
          return;
        }

        NSRect displayFrame = NSMakeRect(0, 0, mainDisplay.width, mainDisplay.height);
        NSImage *capturedImage = [[NSImage alloc] initWithCGImage:cgImage size:displayFrame.size];

        dispatch_async(dispatch_get_main_queue(), ^{
          [self showOverlayWithImage:capturedImage frame:displayFrame];
        });
      }];
    }];
  } else {
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = @"macOS 12.3 or later required";
    alert.informativeText = @"This app requires macOS 12.3 (Monterey) or later for screen capture.";
    [alert runModal];
  }
}

- (void)showOverlayWithImage:(NSImage *)capturedImage frame:(NSRect)frame {
  self.overlayWindow = [[OverlayWindow alloc] initWithContentRect:frame
                                                        styleMask:NSWindowStyleMaskBorderless
                                                          backing:NSBackingStoreBuffered
                                                            defer:NO];

  OverlayView *overlayView = [[OverlayView alloc] initWithFrame:
      NSMakeRect(0, 0, frame.size.width, frame.size.height)];
  overlayView.capturedImage = capturedImage;

  self.overlayWindow.contentView = overlayView;

  [self.overlayWindow makeKeyAndOrderFront:nil];
  [self.overlayWindow makeFirstResponder:overlayView];

  [[NSCursor crosshairCursor] set];
}

- (void)applicationWillTerminate:(NSNotification *)notification {
  if (self.hotKeyRef) {
    UnregisterEventHotKey(self.hotKeyRef);
  }
}

@end

int main(int argc, const char *argv[]) {
  @autoreleasepool {
    NSApplication *app = [NSApplication sharedApplication];
    app.activationPolicy = NSApplicationActivationPolicyAccessory;

    AppDelegate *delegate = [[AppDelegate alloc] init];
    app.delegate = delegate;

    [app run];
  }
  return 0;
}
