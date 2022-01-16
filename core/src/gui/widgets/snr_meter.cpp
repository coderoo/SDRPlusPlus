#include <gui/widgets/volume_meter.h>
#include <algorithm>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

namespace ImGui {

    static const int NLASTSNR = 1500;
    static std::vector<float> lastsnr;

    static ImVec2 postSnrLocation;


    void SNRMeter(float val, const ImVec2& size_arg = ImVec2(0, 0)) {
        ImGuiWindow* window = GetCurrentWindow();
        ImGuiStyle& style = GImGui->Style;

        float pad = style.FramePadding.y;

        ImVec2 min = window->DC.CursorPos;
        ImVec2 size = CalcItemSize(size_arg, CalcItemWidth(), 26);
        ImRect bb(min, min + size);

        ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);

        float lineHeight = size.y;

        ItemSize(size, style.FramePadding.y);
        if (!ItemAdd(bb, 0)) {
            return;
        }

        val = std::clamp<float>(val, 0, 100);
        float ratio = size.x / 90;
        float it = size.x / 9;
        char buf[32];

        float drawVal = (float) val * ratio;

        postSnrLocation = min + ImVec2(0, 50);

        lastsnr.insert(lastsnr.begin(), drawVal);
        if (lastsnr.size() > NLASTSNR)
            lastsnr.resize(NLASTSNR);

        window->DrawList->AddRectFilled(min + ImVec2(0, 1), min + ImVec2(roundf(drawVal), 10), IM_COL32(0, 136, 255, 255));
        window->DrawList->AddLine(min, min + ImVec2(0, 9), text);
        window->DrawList->AddLine(min + ImVec2(0, 9), min + ImVec2(size.x + 1, 9), text);

        for (int i = 0; i < 10; i++) {
            window->DrawList->AddLine(min + ImVec2(roundf((float)i * it), 9), min + ImVec2(roundf((float)i * it), 14), text);
            sprintf(buf, "%d", i * 10);
            ImVec2 sz = ImGui::CalcTextSize(buf);
            window->DrawList->AddText(min + ImVec2(roundf(((float)i * it) - (sz.x / 2.0)) + 1, 16), text, buf);
        }
    }

    static std::vector<float> sma(int smawindow, std::vector <float> &src) {
        float running = 0;
        std::vector<float> dest;
        for(int q=0; q<src.size(); q++) {
            running += src[q];
            float taveraged = 0;
            if (q >= smawindow) {
                running -= src[q-smawindow];
                taveraged = running/smawindow;
            } else {
                taveraged = running/q;
            }
            dest.emplace_back(taveraged);
        }
        return dest;
    }

    static std::vector<float> maxeach(int maxwindow, std::vector <float> &src) {
        float running = 0;
        std::vector<float> dest;
        for(int q=0; q<src.size(); q++) {
            running = std::max(src[q], running);
            if (q % maxwindow == maxwindow - 1 || q == src.size() - 1) {
                dest.emplace_back(running);
                running = 0;
            }
        }
        return dest;
    }

    void SNRMeterAverages() {

        static std::vector<float> r;
        static int counter = 0;
        static const int winsize = 10;
        counter++;
        if (counter % winsize == winsize - 1) {
            r =  maxeach(winsize, lastsnr);
        }
//        maxeach(maxv, 6, smav);
//        maxv = lastsnr;
        ImGuiWindow* window = GetCurrentWindow();
        ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
        for(int q=1; q<r.size(); q++) {
            window->DrawList->AddLine(postSnrLocation + ImVec2(0 + r[q-1], q-1), postSnrLocation + ImVec2(0 + r[q], q), text);
        }

    }
}