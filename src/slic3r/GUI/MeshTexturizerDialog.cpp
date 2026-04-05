#include "MeshTexturizerDialog.hpp"

#include <wx/sizer.h>
#include <wx/statbox.h>

#include "I18N.hpp"
#include "wxExtensions.hpp"

namespace Slic3r { namespace GUI {

MeshTexturizerDialog::MeshTexturizerDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Apply Texture Displacement"),
                wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    build_controls();
    Fit();
    CentreOnParent();
}

void MeshTexturizerDialog::build_controls()
{
    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);

    // ── Texture file ─────────────────────────────────────────────────────────
    {
        auto *box   = new wxStaticBox(this, wxID_ANY, _L("Texture Image"));
        auto *bsizer = new wxStaticBoxSizer(box, wxVERTICAL);

        m_file_picker = new wxFilePickerCtrl(
            this, wxID_ANY, wxEmptyString,
            _L("Select texture image"),
            "Image files (*.png;*.jpg;*.jpeg)|*.png;*.jpg;*.jpeg",
            wxDefaultPosition, wxSize(380, -1),
            wxFLP_OPEN | wxFLP_FILE_MUST_EXIST | wxFLP_USE_TEXTCTRL);

        bsizer->Add(m_file_picker, 0, wxEXPAND | wxALL, 5);
        main_sizer->Add(bsizer, 0, wxEXPAND | wxALL, 5);
    }

    // ── Projection mode ───────────────────────────────────────────────────────
    {
        auto *box   = new wxStaticBox(this, wxID_ANY, _L("Projection Mode"));
        auto *bsizer = new wxStaticBoxSizer(box, wxVERTICAL);

        const wxString modes[] = {
            _L("Planar XY"),
            _L("Planar XZ"),
            _L("Planar YZ"),
            _L("Cylindrical"),
            _L("Spherical"),
            _L("Triplanar (default)"),
            _L("Cubic (Box)"),
        };
        m_mode_choice = new wxChoice(this, wxID_ANY, wxDefaultPosition,
                                     wxDefaultSize, 7, modes);
        m_mode_choice->SetSelection(5); // Triplanar

        bsizer->Add(m_mode_choice, 0, wxEXPAND | wxALL, 5);
        main_sizer->Add(bsizer, 0, wxEXPAND | wxALL, 5);
    }

    // ── UV controls ───────────────────────────────────────────────────────────
    {
        auto *box   = new wxStaticBox(this, wxID_ANY, _L("UV Transform"));
        auto *bsizer = new wxStaticBoxSizer(box, wxVERTICAL);
        auto *grid  = new wxFlexGridSizer(2, 5, 5);
        grid->AddGrowableCol(1);

        auto add_spin = [&](const wxString &label, wxSpinCtrlDouble *&ctrl,
                            double min, double max, double init, double inc,
                            int decimals = 2)
        {
            grid->Add(new wxStaticText(this, wxID_ANY, label), 0,
                      wxALIGN_CENTER_VERTICAL);
            ctrl = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString,
                                        wxDefaultPosition, wxSize(120, -1),
                                        wxSP_ARROW_KEYS, min, max, init, inc);
            ctrl->SetDigits(decimals);
            grid->Add(ctrl, 0, wxEXPAND);
        };

        add_spin(_L("Scale U"),    m_scale_u,   0.05, 10.0, 1.0, 0.05);
        add_spin(_L("Scale V"),    m_scale_v,   0.05, 10.0, 1.0, 0.05);
        add_spin(_L("Offset U"),   m_offset_u,  -10.0, 10.0, 0.0, 0.05);
        add_spin(_L("Offset V"),   m_offset_v,  -10.0, 10.0, 0.0, 0.05);
        add_spin(_L("Rotation °"), m_rotation,  -360.0, 360.0, 0.0, 1.0, 1);

        bsizer->Add(grid, 0, wxEXPAND | wxALL, 5);
        main_sizer->Add(bsizer, 0, wxEXPAND | wxALL, 5);
    }

    // ── Displacement ──────────────────────────────────────────────────────────
    {
        auto *box   = new wxStaticBox(this, wxID_ANY, _L("Displacement"));
        auto *bsizer = new wxStaticBoxSizer(box, wxVERTICAL);
        auto *grid  = new wxFlexGridSizer(2, 5, 5);
        grid->AddGrowableCol(1);

        auto add_spin = [&](const wxString &label, wxSpinCtrlDouble *&ctrl,
                            double min, double max, double init, double inc,
                            int decimals = 2)
        {
            grid->Add(new wxStaticText(this, wxID_ANY, label), 0,
                      wxALIGN_CENTER_VERTICAL);
            ctrl = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString,
                                        wxDefaultPosition, wxSize(120, -1),
                                        wxSP_ARROW_KEYS, min, max, init, inc);
            ctrl->SetDigits(decimals);
            grid->Add(ctrl, 0, wxEXPAND);
        };

        add_spin(_L("Amplitude (mm)"),     m_amplitude, 0.01, 50.0, 1.0, 0.1);
        add_spin(_L("Max edge length (mm)"), m_max_edge, 0.01, 10.0, 0.5, 0.05);

        bsizer->Add(grid, 0, wxEXPAND | wxALL, 5);
        main_sizer->Add(bsizer, 0, wxEXPAND | wxALL, 5);
    }

    // ── Decimation ────────────────────────────────────────────────────────────
    {
        auto *box   = new wxStaticBox(this, wxID_ANY, _L("Decimation"));
        auto *bsizer = new wxStaticBoxSizer(box, wxVERTICAL);
        auto *hrow  = new wxBoxSizer(wxHORIZONTAL);

        m_decimate_cb = new wxCheckBox(this, wxID_ANY, _L("Decimate to:"));
        m_target_tris = new wxSpinCtrl(this, wxID_ANY, "100000",
                                        wxDefaultPosition, wxSize(120, -1),
                                        wxSP_ARROW_KEYS, 1000, 10'000'000, 100000);
        m_target_tris->Enable(false);

        m_decimate_cb->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &e) {
            m_target_tris->Enable(e.IsChecked());
        });

        hrow->Add(m_decimate_cb, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        hrow->Add(m_target_tris, 0, wxALIGN_CENTER_VERTICAL);
        hrow->Add(new wxStaticText(this, wxID_ANY, _L("triangles")), 0,
                  wxALIGN_CENTER_VERTICAL | wxLEFT, 5);

        bsizer->Add(hrow, 0, wxEXPAND | wxALL, 5);
        main_sizer->Add(bsizer, 0, wxEXPAND | wxALL, 5);
    }

    // ── Angle masking ─────────────────────────────────────────────────────────
    {
        auto *box   = new wxStaticBox(this, wxID_ANY, _L("Angle Masking (suppress displacement)"));
        auto *bsizer = new wxStaticBoxSizer(box, wxVERTICAL);
        auto *grid  = new wxFlexGridSizer(2, 5, 5);
        grid->AddGrowableCol(1);

        auto add_spin = [&](const wxString &label, wxSpinCtrlDouble *&ctrl,
                            double init)
        {
            grid->Add(new wxStaticText(this, wxID_ANY, label), 0,
                      wxALIGN_CENTER_VERTICAL);
            ctrl = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString,
                                        wxDefaultPosition, wxSize(100, -1),
                                        wxSP_ARROW_KEYS, 0.0, 90.0, init, 1.0);
            ctrl->SetDigits(0);
            grid->Add(ctrl, 0, wxEXPAND);
        };

        add_spin(_L("Top faces angle limit (°)"),    m_top_angle, 0.0);
        add_spin(_L("Bottom faces angle limit (°)"), m_bot_angle, 0.0);

        bsizer->Add(grid, 0, wxEXPAND | wxALL, 5);
        main_sizer->Add(bsizer, 0, wxEXPAND | wxALL, 5);
    }

    // ── Buttons ───────────────────────────────────────────────────────────────
    {
        auto *btn_sizer = new wxBoxSizer(wxHORIZONTAL);
        btn_sizer->AddStretchSpacer();
        auto *ok_btn = new wxButton(this, wxID_OK, _L("Apply"));
        ok_btn->SetDefault();
        auto *cancel_btn = new wxButton(this, wxID_CANCEL, _L("Cancel"));
        btn_sizer->Add(ok_btn,     0, wxRIGHT, 5);
        btn_sizer->Add(cancel_btn, 0);
        main_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, 8);
    }

    SetSizer(main_sizer);
}

MeshTexturizerParams MeshTexturizerDialog::get_params() const
{
    MeshTexturizerParams p;
    p.mode    = static_cast<MeshTexturizerParams::ProjectionMode>(m_mode_choice->GetSelection());
    p.scale_u = static_cast<float>(m_scale_u->GetValue());
    p.scale_v = static_cast<float>(m_scale_v->GetValue());
    p.offset_u = static_cast<float>(m_offset_u->GetValue());
    p.offset_v = static_cast<float>(m_offset_v->GetValue());
    p.rotation = static_cast<float>(m_rotation->GetValue());
    p.amplitude      = static_cast<float>(m_amplitude->GetValue());
    p.max_edge_length = static_cast<float>(m_max_edge->GetValue());

    if (m_decimate_cb->IsChecked())
        p.target_triangle_count = static_cast<uint32_t>(m_target_tris->GetValue());
    else
        p.target_triangle_count = 0;

    p.top_angle_limit    = static_cast<float>(m_top_angle->GetValue());
    p.bottom_angle_limit = static_cast<float>(m_bot_angle->GetValue());

    return p;
}

std::string MeshTexturizerDialog::get_texture_path() const
{
    return m_file_picker->GetPath().ToUTF8().data();
}

}} // namespace Slic3r::GUI
