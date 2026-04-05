#ifndef slic3r_GUI_MeshTexturizerDialog_hpp_
#define slic3r_GUI_MeshTexturizerDialog_hpp_

#include <wx/wx.h>
#include <wx/filepicker.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/choice.h>

#include "GUI_Utils.hpp"
#include "libslic3r/MeshTexturizer.hpp"

namespace Slic3r { namespace GUI {

/// Dialog for configuring mesh texture displacement parameters.
class MeshTexturizerDialog : public DPIDialog
{
public:
    MeshTexturizerDialog(wxWindow *parent);
    ~MeshTexturizerDialog() override = default;

    /// Return the currently displayed parameters.
    MeshTexturizerParams get_params() const;

    /// Return the selected texture file path (empty string if none).
    std::string get_texture_path() const;

private:
    void build_controls();
    void on_ok(wxCommandEvent &);
    void on_cancel(wxCommandEvent &);
    void on_dpi_changed(const wxRect &) override {}

    // Controls
    wxFilePickerCtrl *m_file_picker   {nullptr};
    wxChoice         *m_mode_choice   {nullptr};
    wxSpinCtrlDouble *m_scale_u       {nullptr};
    wxSpinCtrlDouble *m_scale_v       {nullptr};
    wxSpinCtrlDouble *m_offset_u      {nullptr};
    wxSpinCtrlDouble *m_offset_v      {nullptr};
    wxSpinCtrlDouble *m_rotation      {nullptr};
    wxSpinCtrlDouble *m_amplitude     {nullptr};
    wxSpinCtrlDouble *m_max_edge      {nullptr};
    wxCheckBox       *m_decimate_cb   {nullptr};
    wxSpinCtrl       *m_target_tris   {nullptr};
    wxSpinCtrlDouble *m_top_angle     {nullptr};
    wxSpinCtrlDouble *m_bot_angle     {nullptr};
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_MeshTexturizerDialog_hpp_
