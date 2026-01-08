#include "cursor_sync_common.h"

#include <sintra/logging.h>

#include <QApplication>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QVBoxLayout>
#include <QWidget>

#include <string>

namespace {

class Cursor_sender : public QWidget,
                      public sintra_example::Cursor_sender_bus
{
    Q_OBJECT

signals:
    void cursor_position(int x, int y);

public:
    Cursor_sender()

        :
            sintra_example::Cursor_sender_bus(sintra_example::k_sender_name)
    {
        setWindowTitle("Cursor Sync - SENDER");
        setMinimumSize(400, 300);
        setMouseTracking(true);
        move(100, 100);

        auto* layout = new QVBoxLayout(this);

        m_label = new QLabel("Move mouse here to send cursor position", this);
        m_label->setAlignment(Qt::AlignCenter);
        m_label->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(m_label);

        m_status = new QLabel("Waiting for mouse movement...", this);
        m_status->setAlignment(Qt::AlignCenter);
        m_status->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(m_status);

        connect(this, &Cursor_sender::cursor_position,
                this, &Cursor_sender::on_cursor_sent);
    }

protected:
    void mouseMoveEvent(QMouseEvent* event) override
    {
        emit cursor_position(event->pos().x(), event->pos().y());
        QWidget::mouseMoveEvent(event);
    }

    void paintEvent(QPaintEvent* event) override
    {
        QWidget::paintEvent(event);

        QPainter painter(this);
        painter.setPen(QPen(Qt::blue, 2));
        painter.drawRect(rect().adjusted(2, 2, -2, -2));
    }

private slots:
    void on_cursor_sent(int x, int y)
    {
        using cursor_position_msg = sintra_example::Cursor_sender_bus::cursor_position;

        emit_remote<cursor_position_msg>(x, y);

        ++m_send_count;
        m_status->setText(QString("Sent: (%1, %2)  count=%3")
                              .arg(x)
                              .arg(y)
                              .arg(m_send_count));
    }

private:
    QLabel* m_label = nullptr;
    QLabel* m_status = nullptr;
    int m_send_count = 0;
};

std::string get_receiver_path(const char* argv0)
{
    std::string exe_path(argv0 ? argv0 : "");
    size_t last_sep = exe_path.find_last_of("/\\");
    std::string dir = (last_sep != std::string::npos)
        ? exe_path.substr(0, last_sep + 1)
        : "";

#ifdef _WIN32
    return dir + "sintra_example_qt_cursor_sync_receiver.exe";
#else
    return dir + "sintra_example_qt_cursor_sync_receiver";
#endif
}

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

    const std::string receiver_path = get_receiver_path(argc > 0 ? argv[0] : "");
    if (sintra::spawn_swarm_process(receiver_path) == 0) {
        sintra::Log_stream(sintra::log_level::error)
            << "Failed to spawn receiver process: " << receiver_path << "\n";
        sintra::finalize();
        return 1;
    }

    QApplication app(argc, argv);
    Cursor_sender sender;
    sender.show();

    int result = app.exec();
    sintra::finalize();
    return result;
}

#include "cursor_sync_sender.moc"
