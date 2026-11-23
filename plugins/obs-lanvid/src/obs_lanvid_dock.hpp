#pragma once
// obs_lanvid_dock.hpp — Built-in Qt6 Control Dock ("View -> Docks -> LanVid Control & Tally")
// Dark studio UI matching OBS Studio: Camera Matrix, Manual/Auto Tally, Quality Presets,
// Duplex Intercom (PTT / Latch / All Call / IFB), and Live Telemetry + Panic Reset.

#if defined(LV_HAVE_QT6_WIDGETS) && defined(LV_HAVE_REAL_LIBOBS)

#include <QDockWidget>
#include <QPointer>
#include <QTimer>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QVBoxLayout;

namespace lv::obs {

class LanVidControlDock : public QDockWidget {
public:
    explicit LanVidControlDock(QWidget* parent = nullptr);
    ~LanVidControlDock() override;

private:
    void rebuild_camera_rows();
    void on_tick();

    QTimer* poll_timer_ = nullptr;
    QVBoxLayout* cameras_layout_ = nullptr;
    QComboBox* mic_combo_ = nullptr;
    QComboBox* out_combo_ = nullptr;
    QPushButton* all_call_btn_ = nullptr;
    QSlider* ifb_vol_slider_ = nullptr;
    QProgressBar* operator_vu_bar_ = nullptr;
    QLabel* telemetry_label_ = nullptr;
    bool all_call_active_ = false;
};

void register_lanvid_control_dock();
void unregister_lanvid_control_dock();

}  // namespace lv::obs

#else

namespace lv::obs {
inline void register_lanvid_control_dock() {}
inline void unregister_lanvid_control_dock() {}
}  // namespace lv::obs

#endif
