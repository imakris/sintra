#include "cursor_sync_common.h"

#include <sintra/logging.h>

#include <QApplication>
#include <QLabel>
#include <QMetaObject>
#include <QPainter>
#include <QPen>
#include <QPoint>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <utility>

namespace {

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

class Cursor_receiver : public QWidget,
                        public sintra::Derived_transceiver<Cursor_receiver>
{
    Q_OBJECT

public:
    Cursor_receiver()

        :
            sintra::Derived_transceiver<Cursor_receiver>(sintra_example::k_receiver_name)
    {
        setWindowTitle("Cursor Sync - RECEIVER");
        setMinimumSize(400, 300);
        move(550, 100);

        auto* layout = new QVBoxLayout(this);

        m_label = new QLabel("Waiting for cursor updates from sender...", this);
        m_label->setAlignment(Qt::AlignCenter);
        layout->addWidget(m_label);

        m_status = new QLabel("No data received yet", this);
        m_status->setAlignment(Qt::AlignCenter);
        layout->addWidget(m_status);

        m_blink_timer = new QTimer(this);
        connect(m_blink_timer, &QTimer::timeout, this, [this]() {
            m_blink_state = false;
            update();
        });

        activate(&Cursor_receiver::on_cursor_message,
                 sintra_example::Cursor_sender_bus::named_instance(
                     sintra_example::k_sender_name));
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QWidget::paintEvent(event);

        QPainter painter(this);

        painter.setPen(QPen(Qt::green, 2));
        painter.drawRect(rect().adjusted(2, 2, -2, -2));

        if (m_has_data) {
            painter.setPen(QPen(m_blink_state ? Qt::red : Qt::darkRed, 2));
            painter.drawLine(m_ghost_cursor.x() - 10, m_ghost_cursor.y(),
                             m_ghost_cursor.x() + 10, m_ghost_cursor.y());
            painter.drawLine(m_ghost_cursor.x(), m_ghost_cursor.y() - 10,
                             m_ghost_cursor.x(), m_ghost_cursor.y() + 10);

            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(m_ghost_cursor, 8, 8);
        }
    }

private:
    void on_cursor_message(const sintra_example::Cursor_sender_bus::cursor_position& msg)
    {
        const int x = msg.x;
        const int y = msg.y;

        post_to_ui(this, [x, y](Cursor_receiver* self) {
            self->on_remote_cursor(x, y);
        });
    }

private slots:
    void on_remote_cursor(int x, int y)
    {
        m_ghost_cursor = QPoint(x, y);
        m_has_data = true;
        ++m_recv_count;

        m_status->setText(QString("Received: (%1, %2)  count=%3")
                              .arg(x)
                              .arg(y)
                              .arg(m_recv_count));

        m_blink_state = true;
        m_blink_timer->start(100);
        update();
    }

private:
    QLabel* m_label = nullptr;
    QLabel* m_status = nullptr;
    QTimer* m_blink_timer = nullptr;

    QPoint m_ghost_cursor;
    bool m_has_data = false;
    bool m_blink_state = false;
    int m_recv_count = 0;
};

} // namespace

int main(int argc, char* argv[])
{
    try {
        sintra::init(argc, const_cast<const char* const*>(argv));
    }
    catch (const std::exception& e) {
        sintra::Log_stream(sintra::log_level::error)
            << "Failed to initialize sintra: " << e.what() << "\n";
        return 1;
    }

    QApplication app(argc, argv);
    Cursor_receiver receiver;
    receiver.show();

    int result = app.exec();
    sintra::finalize();
    return result;
}

#include "cursor_sync_receiver.moc"
