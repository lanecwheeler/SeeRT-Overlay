Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Diagnostics;

public class WinDiag {
    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X; public int Y; }

    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT pt);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT pt);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr hWnd, uint gaFlags);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr hWnd, uint uCmd);
    // CharSet.Unicode is required so the StringBuilder is marshalled as LPWSTR.
    // Without it, only the first byte of each UTF-16 char is read back, making
    // class names like "OverlayV2_Output" appear as "O".
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder sb, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr hWnd, StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] public static extern uint GetWindowLong(IntPtr hWnd, int nIndex);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int L, T, R, B; }

    public static string GetWinInfo(IntPtr hwnd) {
        var cls   = new StringBuilder(256);
        var title = new StringBuilder(256);
        GetClassName(hwnd, cls, 256);
        GetWindowText(hwnd, title, 256);
        uint pid = 0;
        GetWindowThreadProcessId(hwnd, out pid);
        string proc = "?";
        try { proc = Process.GetProcessById((int)pid).ProcessName; } catch {}
        uint exStyle = GetWindowLong(hwnd, -20); // GWL_EXSTYLE
        bool isTransparent   = (exStyle & 0x00000020) != 0; // WS_EX_TRANSPARENT
        bool isTopmost       = (exStyle & 0x00000008) != 0; // WS_EX_TOPMOST
        bool isNoActivate    = (exStyle & 0x08000000) != 0; // WS_EX_NOACTIVATE
        bool isLayered       = (exStyle & 0x00080000) != 0; // WS_EX_LAYERED
        RECT r; GetWindowRect(hwnd, out r);
        return string.Format(
            "  HWND={0:X8}  [{1}]  title=\"{2}\"  pid={3}({4})\n" +
            "           rect=({5},{6})-({7},{8})  Transparent={9} Topmost={10} NoActivate={11} Layered={12}",
            hwnd.ToInt64(), cls, title.Length > 60 ? title.ToString().Substring(0,60) : title.ToString(),
            pid, proc, r.L, r.T, r.R, r.B,
            isTransparent, isTopmost, isNoActivate, isLayered);
    }
}
"@

Write-Host ""
Write-Host "=== OverlayV2 process check ===" -ForegroundColor Cyan
$procs = Get-Process -Name "OverlayV2" -ErrorAction SilentlyContinue
if ($procs) {
    foreach ($p in $procs) {
        Write-Host "  Running: PID=$($p.Id)  Started=$($p.StartTime)" -ForegroundColor Yellow
    }
} else {
    Write-Host "  No OverlayV2.exe processes found." -ForegroundColor Green
}

Write-Host ""
Write-Host "Move your cursor over the browser window where clicks are blocked." -ForegroundColor Cyan
Write-Host "Sampling in 5 seconds..." -ForegroundColor Cyan
Start-Sleep -Seconds 5

$pt = New-Object WinDiag+POINT
[WinDiag]::GetCursorPos([ref]$pt) | Out-Null
Write-Host ""
Write-Host "=== Cursor position: $($pt.X), $($pt.Y) ===" -ForegroundColor Cyan

# Window directly under the cursor
$hit = [WinDiag]::WindowFromPoint($pt)
Write-Host ""
Write-Host "WindowFromPoint (topmost non-transparent at cursor):" -ForegroundColor Yellow
if ($hit -ne [IntPtr]::Zero) {
    Write-Host ([WinDiag]::GetWinInfo($hit))
    $root = [WinDiag]::GetAncestor($hit, 2) # GA_ROOT
    if ($root -ne $hit) {
        Write-Host "  --> root HWND:"
        Write-Host ([WinDiag]::GetWinInfo($root))
    }
} else {
    Write-Host "  (none)"
}

# Walk Z-order from top — show the first 10 visible top-level windows
Write-Host ""
Write-Host "=== Top 10 visible top-level windows (Z-order, topmost first) ===" -ForegroundColor Cyan
$hwnd = [WinDiag]::GetWindow([IntPtr]::Zero, 0)  # GW_HWNDFIRST
$count = 0
while ($hwnd -ne [IntPtr]::Zero -and $count -lt 40) {
    if ([WinDiag]::IsWindowVisible($hwnd)) {
        Write-Host ([WinDiag]::GetWinInfo($hwnd))
        $count++
        if ($count -ge 10) { break }
    }
    $hwnd = [WinDiag]::GetWindow($hwnd, 2)  # GW_HWNDNEXT
}

Write-Host ""
Write-Host "Done. Share this output to identify which window is absorbing clicks." -ForegroundColor Green
