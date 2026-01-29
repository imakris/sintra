/**
 * @file multi_cursor_window.cpp
 * @brief Window process for the multi-cursor Qt example.
 *
 * Each window:
 * - Has a unique colored frame
 * - Sends cursor position when mouse is over it
 * - Receives cursor positions from other windows (displayed in sender's color)
 * - Has a "Crash" button for demonstrating recovery
 * - Receives coordinator crash countdown and respawn notifications
 * - Shows countdown during crash recovery
 */

#include "multi_cursor_common.h"

#include <sintra/detail/logging.h>

#include <QApplication>
#include <QCloseEvent>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPoint>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace {

// Helper to safely post UI updates from Sintra message handlers
template<typename Obj, typename Functor>
void post_to_ui(Obj* obj, Functor&& fn)
{
    QPointer<Obj> guard(obj);
    QMetaObject::invokeMethod(
        obj,
        [guard, fn = std::forward<Functor>(fn)]() mutable {
            if (guard) {
                fn(guard.data());
            }
        },
        Qt::QueuedConnection);
}

// Parse window_id from --branch_index (branch_index is 1-based)
int parse_window_id(int argc, char* argv[])
{
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], "--branch_index") == 0) {
            const int branch_index = std::atoi(argv[i + 1]);
            return (branch_index > 0) ? (branch_index - 1) : 0;
        }
    }
    return 0; // Default to window 0
}

// Ghost cursor data from another window
struct Ghost_cursor {
    QPoint position;
    int source_window_id = -1;
    bool visible = false;
};

// State for tracking other windows
struct Window_state {
    bool received_normal_exit = false;
    bool recovering = false;
};


class Cursor_window : public QWidget,
                      public sintra_example::Cursor_bus
{
    Q_OBJECT

signals:
    void cursor_moved(int x, int y);

public:
    explicit Cursor_window(int window_id)
        : sintra_example::Cursor_bus(sintra_example::window_name(window_id))
        , m_window_id(window_id)
    {
        // Window setup
        setWindowTitle(QString("Multi-Cursor Window %1").arg(window_id));
        setMinimumSize(450, 350);
        setMouseTracking(true);

        // Position windows in a 2x2 grid
        int x_offset = (window_id % 2) * 500 + 50;
        int y_offset = (window_id / 2) * 400 + 50;
        move(x_offset, y_offset);

        setup_ui();
        setup_sintra_handlers();
        emit_remote<sintra_example::Cursor_bus::window_hello>(m_window_id);     
    }

    ~Cursor_window() override
    {
        // Deactivate all Sintra handlers before destruction
        deactivate_all();
    }

protected:
    void mouseMoveEvent(QMouseEvent* event) override
    {
        emit cursor_moved(event->pos().x(), event->pos().y());
        QWidget::mouseMoveEvent(event);
    }

    void paintEvent(QPaintEvent* event) override
    {
        QWidget::paintEvent(event);

        QPainter painter(this);

        // Draw our colored border
        const auto& color = sintra_example::k_window_colors[m_window_id];
        painter.setPen(QPen(QColor(color[0], color[1], color[2]), 4));
        painter.drawRect(rect().adjusted(2, 2, -2, -2));

        // Draw ghost cursors from other windows
        for (int i = 0; i < sintra_example::k_num_windows; ++i) {
            if (i == m_window_id) continue;

            const auto& ghost = m_ghost_cursors[i];
            if (ghost.visible) {
                const auto& gc = sintra_example::k_window_colors[ghost.source_window_id];
                QColor ghost_color(gc[0], gc[1], gc[2]);

                // Draw crosshair
                painter.setPen(QPen(ghost_color, 2));
                painter.drawLine(ghost.position.x() - 12, ghost.position.y(),
                                 ghost.position.x() + 12, ghost.position.y());
                painter.drawLine(ghost.position.x(), ghost.position.y() - 12,
                                 ghost.position.x(), ghost.position.y() + 12);

                // Draw circle
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(ghost.position, 10, 10);

                // Draw small label with window number
                painter.setFont(QFont("Arial", 8, QFont::Bold));
                painter.drawText(ghost.position.x() + 14, ghost.position.y() - 6,
                                 QString::number(ghost.source_window_id));
            }
        }
    }

    void closeEvent(QCloseEvent* event) override
    {
        send_goodbye_once();
        QWidget::closeEvent(event);
    }

    void leaveEvent(QEvent* event) override
    {
        using leave_msg = sintra_example::Cursor_bus::cursor_left;
        emit_remote<leave_msg>(m_window_id);
        QWidget::leaveEvent(event);
    }

private slots:
    void on_cursor_sent(int x, int y)
    {
        using cursor_msg = sintra_example::Cursor_bus::cursor_position;
        emit_remote<cursor_msg>(x, y, m_window_id);

        m_status_label->setText(QString("Sent: (%1, %2)").arg(x).arg(y));
    }

    void on_crash_button_clicked()
    {
        sintra::Log_stream(sintra::log_level::info)
            << "[Window " << m_window_id << "] Crash button clicked!\n";

        m_status_label->setText("CRASHING!");

        // Force update and small delay so user sees the message
        repaint();
        QApplication::processEvents();
        QThread::msleep(100);

        // Actually crash - dereference null pointer
        int* null_ptr = nullptr;
        *null_ptr = 42;
    }

private:
    void setup_ui()
    {
        auto* main_layout = new QVBoxLayout(this);

        // Title label
        auto* title_label = new QLabel(QString("Window %1 - Move mouse here").arg(m_window_id), this);
        title_label->setAlignment(Qt::AlignCenter);
        title_label->setFont(QFont("Arial", 14, QFont::Bold));
        title_label->setAttribute(Qt::WA_TransparentForMouseEvents);
        main_layout->addWidget(title_label);

        // Status label
        m_status_label = new QLabel("Ready", this);
        m_status_label->setAlignment(Qt::AlignCenter);
        m_status_label->setAttribute(Qt::WA_TransparentForMouseEvents);
        main_layout->addWidget(m_status_label);

        // Recovery row (one label per window)
        auto* recovery_row = new QWidget(this);
        m_recovery_layout = new QVBoxLayout(recovery_row);
        m_recovery_layout->setContentsMargins(0, 0, 0, 0);
        m_recovery_layout->setSpacing(12);

        for (int i = 0; i < sintra_example::k_num_windows; ++i) {
            auto* label = new QLabel("", recovery_row);
            label->setAlignment(Qt::AlignCenter);
            label->setFont(QFont("Arial", 14, QFont::Bold));
            const auto& c = sintra_example::k_window_colors[i];
            label->setStyleSheet(QString("QLabel { color: rgb(%1,%2,%3); }")
                .arg(c[0]).arg(c[1]).arg(c[2]));
            label->setVisible(false);
            label->setAttribute(Qt::WA_TransparentForMouseEvents);
            m_recovery_labels[i] = label;
            m_recovery_layout->addWidget(label);
        }
        main_layout->addWidget(recovery_row);

        // Notifications label (for normal exits)
        m_notifications_label = new QLabel("", this);
        m_notifications_label->setAlignment(Qt::AlignCenter);
        m_notifications_label->setStyleSheet("QLabel { color: gray; }");
        m_notifications_label->setAttribute(Qt::WA_TransparentForMouseEvents);
        main_layout->addWidget(m_notifications_label);

        // Spacer
        main_layout->addStretch();

        // Button row
        auto* button_layout = new QHBoxLayout();
        button_layout->addStretch();

        // Crash button
        auto* crash_button = new QPushButton("Crash", this);
        crash_button->setStyleSheet(
            "QPushButton { background-color: #d9534f; color: white; "
            "font-weight: bold; padding: 8px 16px; border-radius: 4px; }"
            "QPushButton:hover { background-color: #c9302c; }");
        connect(crash_button, &QPushButton::clicked,
                this, &Cursor_window::on_crash_button_clicked);
        button_layout->addWidget(crash_button);

        button_layout->addStretch();
        main_layout->addLayout(button_layout);

        // Connect local cursor signal
        connect(this, &Cursor_window::cursor_moved,
                this, &Cursor_window::on_cursor_sent);
    }

    void setup_sintra_handlers()
    {
        // Listen for cursor updates from any remote Cursor_bus instance        
        activate(&Cursor_window::on_cursor_message,
                 sintra::Typed_instance_id<sintra_example::Cursor_bus>(sintra::any_remote));

        // Listen for normal exit notifications from any remote Cursor_bus instance
        activate(&Cursor_window::on_normal_exit_notification,
                 sintra::Typed_instance_id<sintra_example::Cursor_bus>(sintra::any_remote));

        // Listen for recovery countdown and respawn notifications
        activate(&Cursor_window::on_recovery_countdown,
                 sintra::Typed_instance_id<sintra_example::Cursor_bus>(sintra::any_remote));
        activate(&Cursor_window::on_recovery_spawned,
                 sintra::Typed_instance_id<sintra_example::Cursor_bus>(sintra::any_remote));

        activate(&Cursor_window::on_cursor_left,
                 sintra::Typed_instance_id<sintra_example::Cursor_bus>(sintra::any_remote));
    }

    void send_goodbye_once()
    {
        if (m_sent_goodbye) {
            return;
        }
        m_sent_goodbye = true;
        emit_remote<sintra_example::Cursor_bus::window_goodbye>(m_window_id);
    }

    // Sintra message handlers
    void on_cursor_message(const sintra_example::Cursor_bus::cursor_position& msg)
    {
        // Ignore our own messages
        if (msg.window_id == m_window_id) return;

        const int x = msg.x;
        const int y = msg.y;
        const int source_id = msg.window_id;

        post_to_ui(this, [x, y, source_id](Cursor_window* self) {
            if (source_id >= 0 && source_id < sintra_example::k_num_windows) {
                auto& ghost = self->m_ghost_cursors[source_id];
                auto& state = self->m_window_states[source_id];

                ghost.position = QPoint(x, y);
                ghost.source_window_id = source_id;
                ghost.visible = true;

                if (state.recovering) {
                    state.recovering = false;
                    self->set_recovery_text(source_id,
                        QString("Window %1 recovered").arg(source_id));
                    self->clear_recovery_text_later(source_id);
                }

                self->update();
            }
        });
    }

    void on_normal_exit_notification(const sintra_example::Cursor_bus::normal_exit_notification& msg)
    {
        const int exited_id = msg.window_id;

        // Ignore our own exit notification
        if (exited_id == m_window_id) return;

        post_to_ui(this, [exited_id](Cursor_window* self) {
            if (exited_id >= 0 && exited_id < sintra_example::k_num_windows) {
                if (self->m_window_states[exited_id].received_normal_exit) {
                    return;
                }
                // Mark as normal exit so we don't treat it as a crash
                self->m_window_states[exited_id].received_normal_exit = true;
                self->m_window_states[exited_id].recovering = false;

                // Hide ghost cursor from exited window
                self->m_ghost_cursors[exited_id].visible = false;
                self->clear_recovery_text(exited_id);
            }

            sintra::Log_stream(sintra::log_level::info)
                << "[Window " << self->m_window_id << "] Window " << exited_id
                << " exited normally\n";

            // Show notification
            QString current = self->m_notifications_label->text();
            if (!current.isEmpty()) {
                current += " | ";
            }
            current += QString("Window %1 exited").arg(exited_id);
            self->m_notifications_label->setText(current);

            self->update();
        });
    }

    void on_recovery_countdown(const sintra_example::Cursor_bus::recovery_countdown& msg)
    {
        const int window_id = msg.window_id;
        const int seconds = msg.seconds_remaining;

        if (window_id == m_window_id) {
            return;
        }

        post_to_ui(this, [window_id, seconds](Cursor_window* self) {
            if (window_id < 0 || window_id >= sintra_example::k_num_windows) {
                return;
            }

            self->m_window_states[window_id].recovering = true;
            self->m_ghost_cursors[window_id].visible = false;

            if (seconds > 0) {
                self->set_recovery_text(window_id,
                    QString("Window %1 in %2s").arg(window_id).arg(seconds));
            }
            else {
                self->set_recovery_text(window_id,
                    QString("Window %1 recovering...").arg(window_id));
            }

            self->update();
        });
    }

    void on_recovery_spawned(const sintra_example::Cursor_bus::recovery_spawned& msg)
    {
        const int window_id = msg.window_id;

        if (window_id == m_window_id) {
            return;
        }

        post_to_ui(this, [window_id](Cursor_window* self) {
            if (window_id < 0 || window_id >= sintra_example::k_num_windows) {
                return;
            }

            self->m_window_states[window_id].recovering = false;
            self->set_recovery_text(window_id,
                QString("Window %1 restarted").arg(window_id));
            self->clear_recovery_text_later(window_id);

            self->update();
        });
    }

    void on_cursor_left(const sintra_example::Cursor_bus::cursor_left& msg)
    {
        const int window_id = msg.window_id;
        if (window_id == m_window_id) {
            return;
        }

        post_to_ui(this, [window_id](Cursor_window* self) {
            if (window_id < 0 || window_id >= sintra_example::k_num_windows) {
                return;
            }

            self->m_ghost_cursors[window_id].visible = false;
            self->update();
        });
    }

    void set_recovery_text(int window_id, const QString& text)
    {
        if (window_id < 0 || window_id >= sintra_example::k_num_windows) {
            return;
        }
        auto* label = m_recovery_labels[window_id];
        if (!label) {
            return;
        }
        label->setText(text);
        label->setVisible(true);
    }

    void clear_recovery_text(int window_id)
    {
        if (window_id < 0 || window_id >= sintra_example::k_num_windows) {
            return;
        }
        auto* label = m_recovery_labels[window_id];
        if (!label) {
            return;
        }
        label->setText("");
        label->setVisible(false);
    }

    void clear_recovery_text_later(int window_id)
    {
        QTimer::singleShot(3000, this, [this, window_id]() {
            clear_recovery_text(window_id);
            update();
        });
    }

private:
    int m_window_id;

    QLabel* m_status_label = nullptr;
    QLabel* m_notifications_label = nullptr;
    QVBoxLayout* m_recovery_layout = nullptr;
    std::array<QLabel*, sintra_example::k_num_windows> m_recovery_labels{};

    std::array<Ghost_cursor, sintra_example::k_num_windows> m_ghost_cursors;
    std::array<Window_state, sintra_example::k_num_windows> m_window_states;
    bool m_sent_goodbye = false;
};

} // namespace


int main(int argc, char* argv[])
{
    // Parse window ID first (before sintra::init consumes args)
    int window_id = parse_window_id(argc, argv);

    try {
        sintra::init(argc, const_cast<const char* const*>(argv));
    }
    catch (const std::exception& e) {
        sintra::Log_stream(sintra::log_level::error)
            << "Failed to initialize sintra: " << e.what() << "\n";
        return 1;
    }

    sintra::enable_recovery();

    sintra::Log_stream(sintra::log_level::info)
        << "[Window " << window_id << "] Starting\n";

    QApplication app(argc, argv);
    Cursor_window window(window_id);
    window.show();

    int result = app.exec();

    sintra::Log_stream(sintra::log_level::info)
        << "[Window " << window_id << "] Exiting with code " << result << "\n";

    sintra::finalize();
    return result;
}

#include "multi_cursor_window.moc"
