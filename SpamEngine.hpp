#pragma once
// Spam Buff - motor de logica pura (sin dependencias de Geode).
// Todos los tiempos estan en milisegundos del reloj del juego (suma de los dt de cada paso).

#include <algorithm>
#include <array>

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

    // Evento real del jugador (lo que llegaria a handleButton).
    void onReal(bool down, double now, bool cubeLike, Config const& c, Emitted& out) {
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
        if (!m_spam) return;
        if (now - m_lastPress > timeoutMs(c)) {
            endSpam(out);
            return;
        }
        if (m_mode != Mode::Pulse) return;
        m_pulseTimer -= dtMs;
        if (m_pulseTimer > 0.0) return;
        m_pulseDown = !m_pulseDown;
        setGame(m_pulseDown, out);
        double step = std::max(m_lastDt, 1.0);
        double down = downPhaseMs(c);
        double phase = m_pulseDown ? down : std::max(periodMs(c) - down, step);
        m_pulseTimer += phase;
        if (m_pulseTimer <= 0.0) m_pulseTimer = phase * 0.5;
    }

    // Mod apagado a mitad de partida: deja el juego en el estado real del dedo.
    void shutdown(Emitted& out) {
        flushPendingUp(out);
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

    double downPhaseMs(Config const& c) const {
        double period = periodMs(c);
        double step = std::max(m_lastDt, 1.0);
        double d = period * double(std::clamp(c.dutyPercent, 10, 90)) / 100.0;
        return std::clamp(d, step, std::max(step, period - step));
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
        m_pulseTimer = downPhaseMs(c);
    }

    void endSpam(Emitted& out) {
        m_spam = false;
        m_pulseDown = false;
        m_pulseTimer = 0.0;
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
