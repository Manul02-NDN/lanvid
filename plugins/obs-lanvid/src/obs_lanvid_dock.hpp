#pragma once
// obs_lanvid_dock.hpp — Built-in Qt6 Studio Control Dock ("View -> Docks -> LanVid Studio")
// Displays active OBS sources and discovered LAN/SHM cameras without UI flicker,
// with Camera Number assignment (CAM 1..16), AUTO/STBY/PVW/PGM Tally, REC, Quality Presets,
// Force IDR, Duplex Intercom (TALK PTT / ALL CALL / IFB), Network Diagnostics & Reset Buffers.

#if defined(LV_HAVE_QT6_WIDGETS) && defined(LV_HAVE_REAL_LIBOBS)

#include <QDockWidget>
#include <QPointer>
#include <QTimer>
#include <cstdint>
#include <string>
#include <vector>

class QComboBox;
class QFrame;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QVBoxLayout;

namespace lv::obs {

class LanVidControlDock : public QDockWidget {
public:
    explicit LanVidControlDock(QWidget* parent = nullptr);
    ~LanVidControlDock() override;

private:
    struct ActiveRowWidgets {
        uint64_t uid = 0;
        uint32_t camera_id = 0;
        QFrame* frame = nullptr;
        QLabel* info_lbl = nullptr;
        QSpinBox* badge_spin = nullptr;
        QComboBox* quality_combo = nullptr;
        QPushButton* auto_btn = nullptr;
        QPushButton* stby_btn = nullptr;
        QPushButton* pvw_btn = nullptr;
        QPushButton* pgm_btn = nullptr;
        QPushButton* rec_btn = nullptr;
        QPushButton* talk_btn = nullptr;
    };

    void sync_camera_rows();
    void on_tick();

    QTimer* poll_timer_ = nullptr;
    QVBoxLayout* cameras_layout_ = nullptr;
    QLabel* discovered_lbl_ = nullptr;
    QComboBox* mic_combo_ = nullptr;
    QComboBox* out_combo_ = nullptr;
    QPushButton* all_call_btn_ = nullptr;
    QSlider* ifb_vol_slider_ = nullptr;
    QProgressBar* operator_vu_bar_ = nullptr;
    QLabel* telemetry_label_ = nullptr;
    QLabel* diag_label_ = nullptr;
    std::vector<ActiveRowWidgets> rows_;
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
