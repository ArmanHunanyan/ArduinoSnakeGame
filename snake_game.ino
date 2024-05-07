#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Max72xxPanel.h>
#include <limits.h>
#include <avr/sleep.h>

enum class Pins : byte {
    ScreenCS = 10,
    ScreenDIN = 11,
    ScreenCLK = 13,
    LeftButton = A7,
    RightButton = A4,
    UpButton = A5,
    DownButton = A2,
    CenterButton =  A1,
    InterruptButton = 2 // can be hardwired to any of the above buttons
};

// The configurations below are intentionally repeated to allow for different settings for different buttons.
struct LeftButtonConfig
{
    static void setup() { pinMode((uint8_t)Pins::LeftButton, INPUT); }
    static bool read() { return analogRead((uint8_t)Pins::LeftButton) < 800; }
};

struct RightButtonConfig
{
    static void setup() { pinMode((uint8_t)Pins::RightButton, INPUT); }
    static bool read() { return analogRead((uint8_t)Pins::RightButton) < 800; }
};

struct UpButtonConfig
{
    static void setup() { pinMode((uint8_t)Pins::UpButton, INPUT); }
    static bool read() { return analogRead((uint8_t)Pins::UpButton) < 800; }
};

struct DownButtonConfig
{
    static void setup() { pinMode((uint8_t)Pins::DownButton, INPUT); }
    static bool read() { return analogRead((uint8_t)Pins::DownButton) < 800; }
};

struct CenterButtonConfig
{
    static void setup() { pinMode((uint8_t)Pins::CenterButton, INPUT); }
    static bool read() { return analogRead((uint8_t)Pins::CenterButton) < 800; }
};

// The values ​​are chosen so that the opposite() function below can be made to work.
enum class Direction : uint8_t {
    Left   = 0,
    Right  = 4,
    Up     = 1,
    Down   = 3,
    Center = 2
};

constexpr Direction opposite(Direction direction) { return Direction(uint8_t(4) - uint8_t(direction));}

constexpr uint8_t mask(Direction direction) { return uint8_t(1) << uint8_t(direction);}

class ShellObject
{
public:
    virtual ~ShellObject() { }
    virtual void onSetup() = 0;
    virtual void onLoop() = 0;
    virtual void onPrepareSleep() = 0;
    virtual void onWakeUp() = 0;
};

class Keypad
{
public:
    using Button = Direction;

    class ButtonSet
    {
    public:
        static ButtonSet read()
        {
            ButtonSet res;
            res.m_flags |= LeftButtonConfig::read()    ? mask(Button::Left)   : 0;
            res.m_flags |= RightButtonConfig::read()   ? mask(Button::Right)  : 0;
            res.m_flags |= UpButtonConfig::read()      ? mask(Button::Up)     : 0;
            res.m_flags |= DownButtonConfig::read()    ? mask(Button::Down)   : 0;
            res.m_flags |= CenterButtonConfig::read()  ? mask(Button::Center) : 0;
            return res;
        }

        bool operator== (ButtonSet rhs) const { return m_flags == rhs.m_flags; }
        bool operator!= (ButtonSet rhs) const { return m_flags != rhs.m_flags; }

        ButtonSet notIn(ButtonSet old) const
        {
            uint8_t diff = m_flags ^ old.m_flags;
            ButtonSet res;
            res.m_flags = diff & m_flags;
            return res;
        }

        ButtonSet difference(ButtonSet old) const
        {
            ButtonSet res;
            res.m_flags = m_flags ^ old.m_flags;
            return res;
        }

        template <typename Func>
        void forEach(Func func)
        {
            for (uint8_t idx = 0; idx < sizeof(m_flags) * 8; ++idx) {
                if ((m_flags & mask(Button(idx))) != 0) {
                    func(Button(idx));
                }
            }
        }

    private:
        uint8_t m_flags = 0;
    };

    enum {
        MaxIdleTime = 15000,
        MaxPressTime = 300
    };

public:
    class Listner
    {
    public:
        virtual ~Listner() { }

        void notifyKeyPress(Keypad::Button btn)
        {
            if (m_flags & EventKeyPress) {
                onKeyPress(btn);
            }
        }
        
        void notifyLongKeyPress(Keypad::Button btn)
        {
            if (m_flags & EventLongKeyPress) {
                onLongKeyPress(btn);
            }
        }

        void notifyKeyRelease(Keypad::Button btn)
        {
            if (m_flags & EventKeyRelease) {
                onKeyRelease(btn);
            }
        }

    private:
        virtual void onKeyPress(Keypad::Button btn) = 0;
        virtual void onLongKeyPress(Keypad::Button btn) = 0;
        virtual void onKeyRelease(Keypad::Button btn) = 0;

    private:
        uint8_t m_flags = 0;

    protected:
        enum EventFlags : uint8_t {
            EventKeyPress = 1,
            EventLongKeyPress = 2,
            EventKeyRelease = 4,
            AllEvents = EventKeyPress | EventLongKeyPress | EventKeyRelease
        };

        void enableEvent(uint8_t flags)
        {
            m_flags |= flags;
        }

        void disableEvent(uint8_t flags)
        {
            m_flags &= ~(flags);
        }
    };

    class IdleListner
    {
    public:
        virtual ~IdleListner() { }
        virtual void onIdle() = 0;
    };    

public:
    Keypad()
        : m_listner(nullptr)
        , m_idleListner(nullptr)
    {
    }

    void setListner(Listner *listner)
    {
        m_listner = listner;
    }

    void setIdleListner(IdleListner *listner)
    {
        m_idleListner = listner;
    }

    void setup()
    {
        LeftButtonConfig::setup();
        RightButtonConfig::setup();
        UpButtonConfig::setup();
        DownButtonConfig::setup();
        CenterButtonConfig::setup();
        m_idleStart = 0;
    }

    void updateIdle(ButtonSet oldState, ButtonSet newState)
    {
        const unsigned long curr = millis();
        if (oldState != newState) {
            m_idleStart = curr;
            return;
        }
        if (m_idleStart == ULONG_MAX) {
            // Do nothing. Idle already notifyed.
        } else {
            const unsigned long idleTime = curr - m_idleStart;
            if (idleTime > MaxIdleTime) {
                if (m_idleListner != nullptr) {
                    m_idleListner->onIdle();
                }
                m_idleStart = ULONG_MAX;
            }
        }
    }

    void loopImpl(ButtonSet oldState, ButtonSet newState)
    {
        updateIdle(oldState, newState);
        /* There is no need for special handling after the idle event.
           If a idle has happened, then oldState == newState.
           Nothing will be executed below */
        auto curr = millis();
        if (m_listner != nullptr) {
            ButtonSet justPressed = newState.notIn(oldState);
            justPressed.forEach([this, curr](Button btn) {
                    m_buttonPressTime[uint8_t(btn)] = curr;
                    m_listner->notifyKeyPress(btn);
                });
            ButtonSet longTimePressed = justPressed.difference(newState);
            longTimePressed.forEach([this, curr] (Button btn) {
                    if (curr - m_buttonPressTime[uint8_t(btn)] > MaxPressTime) {
                        m_listner->notifyLongKeyPress(btn);
                    }
                });
            ButtonSet justReleased = oldState.notIn(newState);
            justReleased.forEach([this] (Button btn) {
                    m_listner->notifyKeyRelease(btn);
                });
        }
    }

    void loop()
    {
        ButtonSet newState = ButtonSet::read();
        loopImpl(m_prevState, newState);
        m_prevState = newState;
    }

    void prepareSleep()
    {

    }

    void weakup()
    {
        m_idleStart = millis();
    }

private:
    Listner *m_listner;
    IdleListner *m_idleListner;
    ButtonSet m_prevState;
    unsigned long m_idleStart;
    unsigned long m_buttonPressTime[5];
};

class Screen
{
public:
    Screen()
        : m_panel((int)Pins::ScreenCS, 2, 2)
    {
    }

    void resetModified()
    {
        m_modified = false;
    }

    inline void drawPixel(int8_t x, int8_t y, uint16_t color)
    {
        m_panel.drawPixel(x, y, color);
        m_modified = true;
    }

    void drawChar(int8_t x, int8_t y, unsigned char ch, uint8_t size)
    {
        m_panel.drawChar(x, y, ch, HIGH, LOW, size);
        m_modified = true;
    }

    void drawCenterText(const char *str)
    {
        int16_t x1, y1, w, h;
        m_panel.setTextSize(0);
        m_panel.setTextWrap(false);
        m_panel.getTextBounds(str, 0, 0, &x1, &y1, &w, &h);
        m_panel.setCursor((16 - w) / 2, (16 - h) / 2);
        m_panel.print(str);
        m_modified = true;
    }

    void drawBitmap(int8_t x, int8_t y, const uint8_t bitmap[], int8_t w,
                  int8_t h)
    {
        m_panel.drawBitmap(x, y, bitmap, w, h, HIGH);
        m_modified = true;
    }

    void clear()
    {
        m_panel.fillScreen(LOW);
        m_modified = true;
    }

    void setup()
    {
        m_panel.fillScreen(LOW);
        m_panel.setIntensity(10);
        m_panel.setPosition(0, 1, 0);
        m_panel.setPosition(1, 1, 1);
        m_panel.setPosition(2, 0, 0);
        m_panel.setPosition(3, 0, 1);
        m_panel.setRotation(1, 0);
        m_panel.setRotation(3, 0);
        m_panel.setRotation(0, 0);
        m_panel.setRotation(2, 0);
    }

    void loop()
    {
        if (m_modified) {
            m_panel.write();
        }
    }

    void prepareSleep()
    {
        clear();
    }

    void weakup()
    {

    }

private:
    Max72xxPanel m_panel;
    bool m_modified;
};

class Shell;

class Application
    : public Keypad::Listner
{
public:
    virtual ~Application()
    { }

    void startup()
    {
        m_loopDelay = 0;
        m_lastLoopTime = ULONG_MAX;
        onStartup();
    }

    void shutdown()
    {
        onShutdown();
    }

    void loop()
    {
        if (m_loopDelay == 0) {
            onLoop();
        } else {
            unsigned long curr = millis();
            if (m_lastLoopTime == ULONG_MAX || (curr - m_lastLoopTime) > m_loopDelay) {
                m_lastLoopTime = curr;
                onLoop();
            }
        }
    }

protected:
    void loopEvery(uint16_t delay)
    {
        m_loopDelay = delay;
    }

    uint16_t timeSinceLastLoop()
    {
        return millis() - m_lastLoopTime;
    }

    uint16_t loopInterval()
    {
        return m_loopDelay;
    }

    void earlyLoop()
    {
        m_lastLoopTime = millis();
        onLoop();       
    }

private:
    virtual void onStartup() = 0;
    virtual void onShutdown() = 0;
    virtual void onLoop() = 0;

public:
    virtual void prepareSleep() = 0;
    virtual void weakup() = 0;

private:
    bool m_done;
    uint16_t m_loopDelay;
    unsigned long m_lastLoopTime;
};

void weakupInterrupt();

class Shell
{
private:
    class IdleListner
        : public Keypad::IdleListner
    {
    public:
        IdleListner(Shell* shell)
            : m_shell(shell)
        { }

        virtual void onIdle()
        {
            m_shell->goToSleep();
        }

        void setup()
        {
        }
    private:
        Shell *m_shell;
    };

public:
    Shell()
        : m_idleListner(this)
    {
        m_keypad.setIdleListner(&m_idleListner);
    }

    void setup()
    {
        pinMode((int8_t)Pins::InterruptButton, INPUT);
        attachInterrupt(digitalPinToInterrupt((int8_t)2), weakupInterrupt, FALLING);
        m_idleListner.setup();
        m_keypad.setup();
        m_screen.setup();
        randomSeed(analogRead(1));
    }

    void sleepWeakup()
    {
        sleep_enable();
        set_sleep_mode(SLEEP_MODE_PWR_DOWN);
        sleep_cpu();
        sleep_disable();
    }

    void loop()
    {
        m_screen.resetModified();
        if (m_weakupNextTime) {
            m_weakupNextTime = false;
            weakup();
        }
        if (m_nextApplication != nullptr) {
            startApplication(m_nextApplication);
            m_nextApplication = nullptr;
        }
        m_keypad.loop();
        if (m_currentApplication != nullptr) {
            m_currentApplication->loop();
        }
        if (m_sleepInitiated) {
            prepareSleep();
        }
        m_screen.loop();
        if (m_sleepInitiated) {
            sleepWeakup();
            m_sleepInitiated = false;
        }
    }

    void prepareSleep()
    {
        m_sleep = true;
        m_keypad.prepareSleep();
        if (m_currentApplication != nullptr) {
            m_currentApplication->prepareSleep();
        }
        m_screen.prepareSleep();
        
    }

    void weakup()
    {
        m_sleep = false;
        m_keypad.weakup();
        m_screen.weakup();
        if (m_currentApplication != nullptr) {
            m_currentApplication->weakup();
        }
    }

    Screen &screen()
    {
        return m_screen;
    }

    void scheduleApplication(Application *app)
    {
        m_nextApplication = app;
    }

    void startApplication(Application *app)
    {
        if (m_currentApplication != nullptr) {
            m_currentApplication->shutdown();
        }
        m_currentApplication = app;
        m_keypad.setListner(app);
        m_currentApplication->startup();
    }

    void weakupNextTime()
    {
        if (m_sleep) {
            m_weakupNextTime = true;
        }
    }

    void goToSleep()
    {
        m_sleepInitiated = true;
    }

private:
    Screen m_screen;
    Keypad m_keypad;
    IdleListner m_idleListner;
    Application *m_currentApplication;
    Application *m_nextApplication = nullptr;
    bool m_weakupNextTime = false;
    bool m_sleep = false;
    bool m_sleepInitiated = false;
};

Shell shell; 

static PROGMEM const uint8_t X_bitmap[] = {B10010000, B01100000, B01100000, B10010000};
static PROGMEM const uint8_t O_bitmap[] = {B11000000, B11000000};
static PROGMEM const uint8_t Continue_bitmap[] = {B10101000, B01010100, B01010100, B10101000};

class WelcomeApplication
    : public Application
{
public:
    void setNextApplication(Application *app)
    {
        m_next = app;
    }

private:
    virtual void onStartup() override
    {
        Keypad::Listner::enableEvent(AllEvents);
        m_frame = 0;
        loopEvery(500);
        shell.screen().clear();
    }

    virtual void onKeyPress(Keypad::Button btn) override
    {
        exit();
    }

    virtual void onLongKeyPress(Keypad::Button btn) override
    {
        exit();
    }

    virtual void onKeyRelease(Keypad::Button btn) override
    {
        exit();
    }

    virtual void prepareSleep() override
    {

    }

    virtual void weakup() override
    {

    }

    void draw(bool a, bool r, bool m, bool o)
    {
        if (a) shell.screen().drawChar(1, 0, 'A', 1);
        if (r) shell.screen().drawChar(10, 0, 'R', 1);
        if (m) shell.screen().drawChar(1, 9, 'M', 1);
        if (o) shell.screen().drawChar(10, 9, 'O', 1);
    }

    virtual void onLoop() override
    {
        if (m_frame == 0) {
            draw(true, false, false, false);
        } else if (m_frame == 1) {
            draw(true, true, false, false);
        } else if (m_frame == 2) {
            draw(true, true, true, false);
        } else if (m_frame == 3) {
            draw(true, true, true, true);
            loopEvery(250);
        } else if (m_frame == 4) {
            shell.screen().clear();
        } else if (m_frame == 5) {
            draw(true, true, true, true);
        } else if (m_frame == 6) {
            shell.screen().clear();
        } else if (m_frame == 7) {
            draw(true, true, true, true);
        } else if (m_frame == 8) {
            shell.screen().clear();
        } else if (m_frame == 9) {
            draw(true, true, true, true);
        } else {
            exit();
        }
        ++m_frame;
    }

    void exit()
    {
        Keypad::Listner::disableEvent(AllEvents);
        shell.scheduleApplication(m_next);
    }

    virtual void onShutdown() override
    {

    }

private:
    uint8_t m_frame;
    Application *m_next;
};


class Point16x16
{
private:
    uint8_t m_data;

public:
    Point16x16()
        : m_data(0)
    {}

    Point16x16(byte x, byte y)
    {
        set(x, y);
    }

    void set(byte x, byte y)
    {
        m_data = 0;
        m_data = (y << byte(4)) | x;
    }

    void setX(byte x)
    {
        m_data &= B11110000;
        m_data |= x;
    }

    void setY(byte y)
    {
        m_data &= B00001111;
        m_data |= (y << byte(4));
    }
    
    byte x() const
    {
        return (m_data & B00001111);
    }

    byte y() const
    {
        return m_data >> byte(4);
    }

    bool operator== (Point16x16 rhs) const
    {
        return m_data == rhs.m_data;
    }

    bool operator!= (Point16x16 rhs) const
    {
        return m_data != rhs.m_data;
    }
};

using Point = Point16x16;

template <uint16_t Cap>
class PointRingVect
{
public:
    PointRingVect()
        : m_front(0)
        , m_back(0)
    { }

    void clear()
    {
        m_front = 0;
        m_back = 0;
    }

    uint8_t size() const
    {
        return ((m_back + Cap) - m_front) % Cap;
    }

    void push_back(Point pt)
    {
        m_points[m_back] = pt;
        ++m_back;
        m_back = m_back % Cap;
    }

    void pop_front()
    {
        ++m_front;
        m_front = m_front % Cap;
    }

    Point operator[] (uint16_t idx) const
    {
        return m_points[(idx + m_front) % Cap];
    }

    Point& operator[] (uint16_t idx)
    {
        return m_points[(idx + m_front) % Cap];
    }

    Point front() const
    {
        return m_points[m_front];
    }

    Point back() const
    {
        uint16_t idx = (m_back + Cap - 1) % Cap;
        return m_points[idx];
    }

private:
    Point m_points[Cap];
    uint16_t m_front;
    uint16_t m_back;
};

class MainMenuApplication
    : public Application
{
public:
    void setNextApplication(Application *app)
    {
        m_next = app;
    }

private:
    virtual void onStartup() override
    {
        Keypad::Listner::enableEvent(AllEvents);
        shell.screen().clear();
        loopEvery(500);
    }

    virtual void onKeyPress(Keypad::Button btn) override
    {
        if (btn == Keypad::Button::Center) {
            exit();
        }
    }

    virtual void onLongKeyPress(Keypad::Button btn) override
    {
    }

    virtual void onKeyRelease(Keypad::Button btn) override
    {
    }

    virtual void onLoop() override
    {
        shell.screen().clear();
        for (int x = 0; x < 2; ++x) {
            for (int y = 0; y < 2; ++y) {
                shell.screen().drawPixel(7 + x, y, HIGH);
                shell.screen().drawPixel(7 + x, 14 + y, HIGH);
                shell.screen().drawPixel(x, 7 + y, HIGH);
                shell.screen().drawPixel(14 + x, 7 + y, HIGH);
            }
        }
        if (m_frame == 0) {
            shell.screen().drawPixel(7, 7, HIGH);
            shell.screen().drawPixel(7, 8, HIGH);
            shell.screen().drawPixel(8, 7, HIGH);
            shell.screen().drawPixel(8, 8, HIGH);
        } else if (m_frame == 1) {
            shell.screen().drawPixel(6, 7, HIGH);
            shell.screen().drawPixel(6, 8, HIGH);
            shell.screen().drawPixel(9, 7, HIGH);
            shell.screen().drawPixel(9, 8, HIGH);
            shell.screen().drawPixel(7, 6, HIGH);
            shell.screen().drawPixel(7, 9, HIGH);
            shell.screen().drawPixel(8, 6, HIGH);
            shell.screen().drawPixel(8, 9, HIGH);
        } else if (m_frame == 2) {
            shell.screen().drawPixel(5, 7, HIGH);
            shell.screen().drawPixel(5, 8, HIGH);
            shell.screen().drawPixel(10, 7, HIGH);
            shell.screen().drawPixel(10, 8, HIGH);
            shell.screen().drawPixel(7, 5, HIGH);
            shell.screen().drawPixel(7, 10, HIGH);
            shell.screen().drawPixel(8, 5, HIGH);
            shell.screen().drawPixel(8, 10, HIGH);
        }
        m_frame = ++m_frame % 3;
    }

    void exit()
    {
        Keypad::Listner::disableEvent(AllEvents);
        shell.scheduleApplication(m_next);
    }

    virtual void onShutdown() override
    {
    }

    virtual void prepareSleep() override
    {

    }

    virtual void weakup() override
    {
        
    }

private:
    Application *m_next;
    byte m_frame = 0;
};

class Board16x16
{
public:
    Board16x16()
    {
        memset(m_bits, 0, sizeof(m_bits) * sizeof(m_bits[0]));
    }

    inline void set(const PointRingVect<256> &pts)
    {
        for (uint16_t idx = 0, count = pts.size(); idx < count; ++idx) {
            m_bits[pts[idx].y()] |= (uint16_t(1) << pts[idx].x());
        }
    }

    void set(Point pt)
    {
        m_bits[pt.y()] |= (uint16_t(1) << pt.x());
    }

    Point findFreePoint(Point seed) 
    {
        for (uint8_t x = seed.x(); x < 16; ++x) {
            for (uint8_t y = seed.y(); y < 16; ++y) {
                if ((m_bits[y] & (uint16_t(1) << x)) == 0) {
                    return Point(x, y);
                }
            }
        }
        for (uint8_t x = 0, xc = seed.x(); x < xc; ++x) {
            for (uint8_t y = 0, yc = seed.y(); y < yc; ++y) {
                if ((m_bits[y] & (uint16_t(1) << x)) == 0) {
                    return Point(x, y);
                }
            }
        }
        return Point(0, 0);
    }

private:
    uint16_t m_bits[16];
};

class GameOverApplication;

class SnakeGameApplication
    : public Application
{
public:
    void setGameOverApplication(GameOverApplication *app)
    {
        m_gameOverApp = app;
    }

    void setPauseGameApplication(Application *app)
    {
        m_pauseGameApp = app;
    }

    void doNotReset()
    {
        m_doNotReset = true;
    }

private:
    using MoveDirection = Direction;

    virtual void onStartup() override
    {
        Keypad::Listner::enableEvent(AllEvents);
        startGame();
        m_acceptEventsUntillLoop = true;
    }

    void startGame()
    {
        shell.screen().clear();
        loopEvery(320);
        if (!m_doNotReset) {
            m_snake.clear();
            m_snake.push_back(Point(6, 7));
            m_snake.push_back(Point(7, 7));
            m_snake.push_back(Point(8, 7));

            makeApple();
            m_direction = MoveDirection::Right;
        }
        drawSnakeAndApple();
    }

    void makeApple()
    {
        uint16_t rnd = random(256);
        uint8_t x, y;
        x = rnd / 16;
        y = rnd % 16;
        static Board16x16 board;
        board.set(m_snake);
        m_apple = board.findFreePoint(Point(x, y));
    }

    void drawSnakeAndApple()
    {
        for (uint16_t idx = 0; idx < m_snake.size(); ++idx) {
            shell.screen().drawPixel(m_snake[idx].x(), m_snake[idx].y(), HIGH);
        }
        shell.screen().drawPixel(m_apple.x(), m_apple.y(), HIGH);
    }

    virtual void onKeyPress(Keypad::Button btn) override
    {
        if (btn == Keypad::Button::Center) {
            Keypad::Listner::disableEvent(AllEvents);
            shell.scheduleApplication(m_pauseGameApp);
            return;
        }

        if (!m_acceptEventsUntillLoop) {
            return;
        }

        if (opposite(btn) == m_direction) {
            return;
        }
        m_direction = btn;
        uint16_t speed = loopInterval();
        uint16_t timeSince = timeSinceLastLoop();
        if ((timeSince - speed) > (speed / 10)) {
            earlyLoop();
        } else {
            m_acceptEventsUntillLoop = false;
        }
    }

    virtual void onLongKeyPress(Keypad::Button btn) override
    {
        if (btn == Keypad::Button::Center) {
            return;
        }
        if (opposite(btn) == m_direction) {
            return;
        }
        loopEvery(50);
    }

    virtual void onKeyRelease(Keypad::Button btn) override
    {
        loopEvery(320);
    }

    void roll()
    {
        Point newPt, oldTail;
        MoveRes moveRes = moveSnake(newPt, oldTail);
        shell.screen().drawPixel(newPt.x(), newPt.y(), HIGH);
        if (moveRes == MoveRes::Regular) {
            shell.screen().drawPixel(oldTail.x(), oldTail.y(), LOW);
        } else if (moveRes == MoveRes::AppleHit) {
            makeApple();
            shell.screen().drawPixel(m_apple.x(), m_apple.y(), HIGH);
        } else if (moveRes == MoveRes::SelfHit) {
            gameOver();
        }
    }

    virtual void onLoop() override
    {
        roll();
        m_acceptEventsUntillLoop = true;
    }

    enum MoveRes : uint8_t {
        Regular,
        AppleHit,
        SelfHit
    };

    MoveRes moveSnake(Point &fill, Point &clear)
    {
        clear = m_snake.front();
        fill = m_snake.back();
        switch (m_direction) {
            case MoveDirection::Left:
                fill.setX((fill.x() + 16 - 1) % 16);
                break;
            case MoveDirection::Right:
                fill.setX((fill.x() + 1) % 16);
                break;
            case MoveDirection::Up:
                fill.setY((fill.y() + 16 - 1) % 16);
                break;
            case MoveDirection::Down:
                fill.setY((fill.y() + 1) % 16);
                break;
        }

        for (uint16_t idx = 0, count = m_snake.size(); idx < count; ++idx) {
            if (fill == m_snake[idx]) {
                return MoveRes::SelfHit;
            }
        }
        m_snake.push_back(fill);
        if (fill == m_apple) {
            return MoveRes::AppleHit;
        }
        m_snake.pop_front();
        return MoveRes::Regular;
    }

    void gameOver();

    virtual void onShutdown() override
    {
    }

    virtual void prepareSleep() override
    {

    }

    virtual void weakup() override
    {
        drawSnakeAndApple();
    }

private:
    GameOverApplication *m_gameOverApp;
    Application *m_pauseGameApp;
    PointRingVect<256> m_snake;
    MoveDirection m_direction;
    Point m_apple;
    bool m_acceptEventsUntillLoop = true;
    bool m_doNotReset = false;
};

class GameOverApplication
    : public Application
{
public:
    void setNextApplication(Application *app)
    {
        m_nextApp = app;
    }

    void setSnake(PointRingVect<256> *snake)
    {
        m_snake = snake;
    }

private:
    virtual void onStartup() override
    {
        Keypad::Listner::enableEvent(AllEvents);
        shell.screen().clear();
        loopEvery(400);
        m_frame = 0;
    }

    virtual void onKeyPress(Keypad::Button btn) override
    {
        if (m_frame > 6) {
            exit();
        }
    }

    virtual void onLongKeyPress(Keypad::Button btn) override
    {
        if (m_frame > 6) {
            exit();
        }
    }

    virtual void onKeyRelease(Keypad::Button btn) override
    {
        if (m_frame > 6) {
            exit();
        }
    }

    virtual void onLoop() override
    {
        shell.screen().clear();
        if (m_frame > 6) {
            uint16_t len = m_snake->size();
            char txt[4] = {0, 0, 0, 0};
            if (len < 10) {
                txt[0] = '0' + char(len);
            } else if (len < 100) {
                txt[0] = '0' + char(len / 10);
                txt[1] = '0' + char(len % 10);
            } else {
                txt[0] = '0' + char(len / 100);
                txt[1] = '0' + char((len % 100) / 10);
                txt[2] = '0' + char(len % 10);
            }

            shell.screen().drawCenterText(txt);
            return;
        }
        for (int idx = 0, cnt = m_snake->size(); idx < cnt; ++idx) {
            Point pt = (*m_snake)[idx];
            shell.screen().drawPixel(pt.x(), pt.y(), m_frame % 2 == 0 ? LOW : HIGH);
        }
        ++m_frame;
        loopEvery(loopInterval() - (loopInterval() / 10));
    }

    void exit()
    {
        Keypad::Listner::disableEvent(AllEvents);
        shell.scheduleApplication(m_nextApp);
    }

    virtual void onShutdown() override
    {
    }

    virtual void prepareSleep() override
    {
    }

    virtual void weakup() override
    {
    }

private:
    Application *m_nextApp;
    byte m_frame = 0;
    PointRingVect<256> *m_snake;
};

void SnakeGameApplication::gameOver()
{
    Keypad::Listner::disableEvent(AllEvents);
    m_gameOverApp->setSnake(&m_snake);
    shell.scheduleApplication(m_gameOverApp);
}

class PauseGameApplication
    : public Application
{
public:
    void setSnakeGameApplication(SnakeGameApplication *app)
    {
        m_snakeGameApp = app;
    }

    void setMainMenuApplication(MainMenuApplication *app)
    {
        m_mainMenuApp = app;
    }

private:
    virtual void onStartup() override
    {
        Keypad::Listner::enableEvent(AllEvents);
        shell.screen().clear();
        loopEvery(400);
        m_frame = 0;
    }

    virtual void onKeyPress(Keypad::Button btn) override
    {
        if (btn == Keypad::Button::Center) {
            continueToSnakeGame();
        } else {
            exitToMainMenu();
        }
    }

    virtual void onLongKeyPress(Keypad::Button btn) override
    {
    }

    virtual void onKeyRelease(Keypad::Button btn) override
    {
    }

    virtual void onLoop() override
    {
        shell.screen().clear();
        shell.screen().drawBitmap(6, 12, X_bitmap, 4, 4);
        shell.screen().drawBitmap(0, 7, O_bitmap, 2, 2);
        shell.screen().drawBitmap(14, 7, O_bitmap, 2, 2);
        shell.screen().drawBitmap(7, 0, O_bitmap, 2, 2);
        shell.screen().drawBitmap(5, 6, Continue_bitmap, m_frame * 2, 4);
        m_frame = (m_frame + 1) % 4;
    }

    void exitToMainMenu()
    {
        Keypad::Listner::disableEvent(AllEvents);
        shell.scheduleApplication(m_mainMenuApp);
    }

    void continueToSnakeGame()
    {
        Keypad::Listner::disableEvent(AllEvents);
        m_snakeGameApp->doNotReset();
        shell.scheduleApplication(m_snakeGameApp);
    }

    virtual void onShutdown() override
    {
    }

    virtual void prepareSleep() override
    {
    }

    virtual void weakup() override
    {
        shell.screen().clear();
    }

private:
    SnakeGameApplication *m_snakeGameApp;
    MainMenuApplication *m_mainMenuApp;
    uint8_t m_frame;
};

class ApplicationLauncher
{
public:
    void setup()
    {
        m_welcomeApp.setNextApplication(&m_mainMenuApp);
        m_mainMenuApp.setNextApplication(&m_snakeApp);
        m_gameOverApp.setNextApplication(&m_mainMenuApp);
        m_snakeApp.setGameOverApplication(&m_gameOverApp);
        m_snakeApp.setPauseGameApplication(&m_pauseGameApp);
        m_pauseGameApp.setSnakeGameApplication(&m_snakeApp);
        m_pauseGameApp.setMainMenuApplication(&m_mainMenuApp);
        shell.scheduleApplication(&m_welcomeApp);
    }

private:
    WelcomeApplication m_welcomeApp;
    MainMenuApplication m_mainMenuApp;
    SnakeGameApplication m_snakeApp;
    GameOverApplication m_gameOverApp;
    PauseGameApplication m_pauseGameApp;
};

ApplicationLauncher appLauncher;

void weakupInterrupt()
{
    shell.weakupNextTime();
}

void setup() {
    Serial.begin(9600);
    Serial.println(F("----------------"));
    shell.setup();
    appLauncher.setup();
}

void loop() {
    shell.loop();
}