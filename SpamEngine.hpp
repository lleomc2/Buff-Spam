#pragma once
// Spam Buff - motor de logica pura (sin dependencias de Geode).
// Todos los tiempos estan en milisegundos del reloj del juego (suma de los dt de cada paso).

#include <algorithm>
#include <array>
#include <cmath>

namespace spambuff {

enum class Mode { Auto, Hold, Pulse };

struct Config {
    bool   enabled     = true;
    Mode   mode        = Mode::Auto;
    double triggerCps  = 7.0;   // CPS minimos para considerar que es spam
    int    minTaps     = 4;     // toques seguidos a esa velocidad para activar
    bool   speedAuto   = true;  // Pulso: copiar tu ritmo
    double spamCps     = 14.0;  // Pulso: velocidad fija
    int    precision   = 6;     // 1..10 (alta = corta antes al dejar de tocar)
    int    dutyPercent = 50;    // Pulso: % del ciclo presionado
    bool   protectTaps = true;  // alarga toques ultra cortos
    double minPressMs  = 6.0;
    double autoIntervalMs = 70.0; // auto-spam: tiempo entre clics (ms)
    int    autoDutyPercent = 55;  // auto-spam: % del ciclo apretado (punto de partida si nivela)
    bool   level       = true;    // mantener recto (nave / wave): ajusta solo cuanto aprieta
    // ganancias del nivelador (calibradas con simulaciones, no son ajustes del jugador)
    double shipKp = 0.050, shipKi = 0.006, shipKpos = 0.030; // nave: la altura reacciona con inercia
    double waveKp = 0.020, waveKi = 0.006, waveKpos = 0.100; // wave: la altura reacciona directo
};

// Lo que el motor necesita saber del jugador para mantenerlo recto.
struct Feedback {
    bool   valid      = false;
    int    kind       = 0;     // 0 = no nivelar, 1 = nave, 2 = wave (ahi apretar / soltar cambia la altura)
    bool   upsideDown = false;
    double y          = 0.0;   // altura del jugador (unidades de GD)
};

// Eventos que hay que entregar al juego, en orden (true = presionar, false = soltar).
struct Emitted {
    std::array<bool, 6> down{};
    int n = 0;
    void push(bool d) {
        if (n < static_cast<int>(down.size())) down[n++] = d;
    }
};

class Channel {
public:
    void reset() { *this = Channel{}; }

    bool gameDown() const { return m_gameDown; }
    bool spamActive() const { return m_spam; }
    bool wantsRelease() const { return m_gameDown && m_fingers == 0; }
    int  fingers() const { return m_fingers; }
    Mode activeMode() const { return m_mode; }
    bool autoActive() const { return m_auto; }
    double levelDuty() const { return m_lv.on ? m_lv.duty : -1.0; }

    // Estado del jugador en este paso (altura, gravedad, si es nave / wave).
    void feed(Feedback const& fb) { m_fb = fb; }

    // Auto-spam (tecla o boton): genera los clics solo, a intervalo fijo.
    // Mientras esta activo, los toques reales solo se cuentan (no llegan al juego).
    void setAuto(bool on, Config const& c, Emitted& out) {
        if (on == m_auto) return;
        if (on) {
            flushPendingUp(out);
            m_spam = false;
            m_n = 0;
            m_pulseDown = false;
            m_pulseTimer = 0.0;
            m_auto = true;
            m_autoDown = true;
            levelBegin(c, double(c.autoDutyPercent) / 100.0);
            if (!m_gameDown) setGame(true, out);
            m_autoTimer = autoPhase(true, c);
        } else {
            m_auto = false;
            m_autoDown = false;
            m_autoTimer = 0.0;
            m_lv = Leveler{};
            bool want = m_fingers > 0; // si el dedo sigue abajo, queda como hold normal
            if (m_gameDown != want) setGame(want, out);
        }
    }

    // Evento real del jugador (lo que llegaria a handleButton).
    void onReal(bool down, double now, bool cubeLike, Config const& c, Emitted& out) {
        if (m_auto) {
            if (down) ++m_fingers;
            else if (m_fingers > 0) --m_fingers;
            return;
        }
        if (down) {
            ++m_fingers;
            pushPress(now);
            if (m_spam) {
                double iv = now - m_lastPress;
                if (iv > 8.0) m_avg = std::clamp(m_avg * 0.6 + iv * 0.4, 25.0, 400.0);
                m_lastPress = now;
                return; // con spam activo manda el motor
            }
            if (looksLikeSpam(c)) {
                startSpam(now, cubeLike, c, out);
                return;
            }
            flushPendingUp(out); // orden correcto: primero el soltar atrasado
            setGame(true, out);
            m_pressedAt = now;
        } else {
            if (m_fingers > 0) --m_fingers;
            if (m_spam) return;
            if (c.protectTaps && m_gameDown && !m_pendingUp) {
                double held = now - m_pressedAt;
                if (held < c.minPressMs) {
                    m_pendingUp = true;
                    m_upDue = m_pressedAt + c.minPressMs;
                    return;
                }
            }
            m_pendingUp = false;
            setGame(false, out);
        }
    }

    // Un paso de fisica (se llama antes de procesar el paso).
    void tick(double now, double dtMs, Config const& c, Emitted& out) {
        if (dtMs > 0.0) m_lastDt = dtMs;
        if (m_pendingUp && now >= m_upDue) flushPendingUp(out);
        if (m_auto) {
            levelTick(dtMs);
            m_autoTimer -= dtMs;
            if (m_autoTimer > 0.0) return;
            m_autoDown = !m_autoDown;
            if (m_autoDown) levelCycle(c); // ciclo nuevo: reajusta cuanto aprieta
            setGame(m_autoDown, out);
            double phase = autoPhase(m_autoDown, c);
            m_autoTimer += phase;
            if (m_autoTimer <= 0.0) m_autoTimer = phase * 0.5;
            return;
        }
        if (!m_spam) return;
        if (now - m_lastPress > timeoutMs(c)) {
            endSpam(out);
            return;
        }
        if (m_mode != Mode::Pulse) return;
        levelTick(dtMs);
        m_pulseTimer -= dtMs;
        if (m_pulseTimer > 0.0) return;
        m_pulseDown = !m_pulseDown;
        if (m_pulseDown) levelCycle(c);
        setGame(m_pulseDown, out);
        double phase = pulsePhase(m_pulseDown, c);
        m_pulseTimer += phase;
        if (m_pulseTimer <= 0.0) m_pulseTimer = phase * 0.5;
    }

    // Mod apagado a mitad de partida: deja el juego en el estado real del dedo.
    void shutdown(Emitted& out) {
        flushPendingUp(out);
        if (m_auto) setAuto(false, Config{}, out);
        if (m_spam) endSpam(out);
        m_n = 0;
    }

private:
    static constexpr int kHist = 8;

    int    m_fingers   = 0;      // presionadas reales menos soltadas reales
    bool   m_gameDown  = false;  // ultimo estado entregado al juego
    std::array<double, kHist> m_times{};
    int    m_n         = 0;
    double m_lastPress = -1e9;
    double m_avg       = 140.0;  // intervalo medio entre toques (ms)
    double m_pressedAt = -1e9;
    bool   m_pendingUp = false;
    double m_upDue     = 0.0;
    bool   m_spam      = false;
    Mode   m_mode      = Mode::Hold;
    bool   m_pulseDown = false;
    double m_pulseTimer = 0.0;
    double m_lastDt    = 1000.0 / 240.0;
    bool   m_auto      = false;
    bool   m_autoDown  = false;
    double m_autoTimer = 0.0;

    // --- nivelador: mantiene la altura aprendiendo cuanto hay que apretar en cada ciclo
    struct Leveler {
        bool   on = false;
        int    kind = 0;
        bool   upsideDown = false;
        double refY = 0.0;      // altura que se quiere mantener
        double startY = 0.0;    // altura al empezar el ciclo
        double lastY = 0.0;
        double elapsedMs = 0.0; // duracion del ciclo en curso
        double initial = 0.5;
        double base = 0.5;      // % apretado aprendido (el que da altura constante)
        double duty = 0.5;      // % apretado que se usa en el ciclo en curso
    };
    Leveler  m_lv;
    Feedback m_fb;

    double dutyNow(double fixedDuty) const { return m_lv.on ? m_lv.duty : fixedDuty; }

    void levelBegin(Config const& c, double initialDuty) {
        m_lv = Leveler{};
        if (!c.level || !m_fb.valid || m_fb.kind == 0) return;
        m_lv.on = true;
        m_lv.kind = m_fb.kind;
        m_lv.upsideDown = m_fb.upsideDown;
        m_lv.refY = m_lv.startY = m_lv.lastY = m_fb.y;
        // en la wave el punto de equilibrio es exactamente 50% (sube y baja a la misma velocidad)
        double start = (m_fb.kind == 2) ? 0.5 : initialDuty;
        m_lv.initial = m_lv.base = m_lv.duty = std::clamp(start, 0.2, 0.9);
    }

    void levelTick(double dtMs) {
        if (m_lv.on) m_lv.elapsedMs += dtMs;
    }

    // Se llama al empezar cada ciclo (cuando vuelve a apretar).
    void levelCycle(Config const& c) {
        if (!m_lv.on) return;
        if (!c.level || !m_fb.valid || m_fb.kind == 0) {
            m_lv.on = false; // cambio a otro modo (cubo, ufo...): vuelve al % fijo
            return;
        }
        double y = m_fb.y;
        double frames = std::max(m_lv.elapsedMs / (1000.0 / 60.0), 0.5);
        double jump = 150.0 + 20.0 * frames;
        if (m_fb.kind != m_lv.kind || m_fb.upsideDown != m_lv.upsideDown || std::fabs(y - m_lv.lastY) > jump) {
            // cambio de vehiculo, gravedad invertida o salto grande de posicion (portal): empezar de nuevo
            m_lv.kind = m_fb.kind;
            m_lv.upsideDown = m_fb.upsideDown;
            m_lv.refY = m_lv.startY = m_lv.lastY = y;
            m_lv.elapsedMs = 0.0;
            m_lv.initial = (m_lv.kind == 2) ? 0.5 : std::clamp(double(c.autoDutyPercent) / 100.0, 0.2, 0.9);
            m_lv.base = m_lv.duty = m_lv.initial;
            return;
        }
        bool wave = m_lv.kind == 2;
        double kp = wave ? c.waveKp : c.shipKp;
        double ki = wave ? c.waveKi : c.shipKi;
        double kpos = wave ? c.waveKpos : c.shipKpos;
        double sign = m_fb.upsideDown ? -1.0 : 1.0;     // + = en el sentido que empuja apretar
        double vEst = (y - m_lv.startY) / frames * sign; // unidades por cuadro a 60 Hz
        double posErr = (y - m_lv.refY) * sign;
        double e = vEst + kpos * posErr;
        m_lv.base = std::clamp(m_lv.base - ki * e, 0.2, 0.9);
        m_lv.duty = std::clamp(m_lv.base - kp * e, 0.10, 0.92);
        m_lv.startY = m_lv.lastY = y;
        m_lv.elapsedMs = 0.0;
    }

    // La fase mas corta no puede durar menos de un paso de fisica. Si el % que pide el nivelador
    // no cabe en el intervalo, se estira un poco el intervalo (menos CPS) en vez de perder el control.
    static double stretchForDuty(double interval, double duty, double step) {
        double share = std::clamp(std::min(duty, 1.0 - duty), 0.06, 0.5);
        return std::max(interval, step / share);
    }

    double autoPhase(bool down, Config const& c) const {
        double step = std::max(m_lastDt, 1.0);
        double interval = std::max(c.autoIntervalMs, step * 2.0);
        double duty = dutyNow(double(c.autoDutyPercent) / 100.0);
        interval = stretchForDuty(interval, duty, step);
        double press = std::clamp(interval * duty, step, std::max(step, interval - step));
        return down ? press : std::max(interval - press, step);
    }

    double pulsePhase(bool down, Config const& c) const {
        double step = std::max(m_lastDt, 1.0);
        double period = periodMs(c);
        double duty = dutyNow(double(std::clamp(c.dutyPercent, 10, 90)) / 100.0);
        period = stretchForDuty(period, duty, step);
        double press = std::clamp(period * duty, step, std::max(step, period - step));
        return down ? press : std::max(period - press, step);
    }

    void setGame(bool d, Emitted& out) {
        m_gameDown = d;
        out.push(d);
    }

    void flushPendingUp(Emitted& out) {
        if (m_pendingUp) {
            m_pendingUp = false;
            setGame(false, out);
        }
    }

    void pushPress(double now) {
        if (m_n == kHist) {
            std::move(m_times.begin() + 1, m_times.end(), m_times.begin());
            --m_n;
        }
        m_times[m_n++] = now;
    }

    bool looksLikeSpam(Config const& c) const {
        int need = std::clamp(c.minTaps, 2, kHist);
        if (m_n < need) return false;
        double maxI = 1000.0 / std::max(1.0, c.triggerCps);
        double mean = (m_times[m_n - 1] - m_times[m_n - need]) / double(need - 1);
        if (mean > maxI) return false;
        for (int i = m_n - need + 1; i < m_n; ++i) {
            if (m_times[i] - m_times[i - 1] > maxI * 1.8) return false;
        }
        return true;
    }

    double timeoutMs(Config const& c) const {
        double p = double(std::clamp(c.precision, 1, 10));
        double f = 2.5 - (p - 1.0) * (1.2 / 9.0);
        return std::clamp(m_avg * f, 45.0, 400.0);
    }

    double periodMs(Config const& c) const {
        double cps = c.speedAuto ? 1000.0 / std::max(m_avg, 1.0) : c.spamCps;
        cps = std::clamp(cps, 4.0, 30.0);
        return 1000.0 / cps;
    }

    void startSpam(double now, bool cubeLike, Config const& c, Emitted& out) {
        m_spam = true;
        m_mode = (c.mode == Mode::Auto) ? (cubeLike ? Mode::Hold : Mode::Pulse) : c.mode;
        int need = std::clamp(c.minTaps, 2, kHist);
        double mean = (m_times[m_n - 1] - m_times[m_n - need]) / double(need - 1);
        m_avg = std::clamp(mean, 25.0, 400.0);
        m_lastPress = now;
        flushPendingUp(out);
        if (!m_gameDown) {
            setGame(true, out);
            m_pressedAt = now;
        }
        m_pulseDown = true;
        if (m_mode == Mode::Pulse) levelBegin(c, double(std::clamp(c.dutyPercent, 10, 90)) / 100.0);
        m_pulseTimer = pulsePhase(true, c);
    }

    void endSpam(Emitted& out) {
        m_spam = false;
        m_pulseDown = false;
        m_pulseTimer = 0.0;
        m_lv = Leveler{};
        bool want = m_fingers > 0; // si el dedo sigue abajo, queda como hold normal
        if (m_gameDown != want) setGame(want, out);
        m_n = 0;
    }
};

struct Engine {
    std::array<Channel, 2> ch; // 0 = jugador 1, 1 = jugador 2
    double now = 0.0;

    Channel& channel(bool isPlayer1) { return ch[isPlayer1 ? 0 : 1]; }
    bool anySpam() const { return ch[0].spamActive() || ch[1].spamActive(); }
    void reset() {
        for (auto& c : ch) c.reset();
    }
};

} // namespace spambuff
