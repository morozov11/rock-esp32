param([switch]$SetToSpeakers)
$src = @'
using System;
using System.Runtime.InteropServices;

public class AudioEndpoints {
    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
    class MMDeviceEnumeratorCom {}

    [Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceEnumerator {
        int EnumAudioEndpoints(int dataFlow, int stateMask, out IMMDeviceCollection devices);
        int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice device);
    }

    [Guid("0BD7A1BE-7A1A-44DB-8397CC369451FD14".Replace("FD14", "FD14")), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceCollection {
        int GetCount(out int count);
        int Item(int index, out IMMDevice device);
    }

    [Guid("D666063F-1587-4E43-81F1B948EC4E2215".Replace("2215", "2215")), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDevice {
        int Activate(ref Guid iid, int clsCtx, IntPtr activationParams, out object iface);
        int OpenPropertyStore(int access, out IPropertyStore properties);
        int GetId(out IntPtr id);
    }

    [Guid("886d8eeb-8cf2-4446-8d02cdaba517fdc2".Replace("fdc2","fdc2")), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPropertyStore {
        int GetCount(out int count);
        int GetAt(int index, out PROPERTYKEY key);
        int GetValue(ref PROPERTYKEY key, out PROPVARIANT value);
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PROPERTYKEY { public Guid fmtid; public int pid; }

    [StructLayout(LayoutKind.Explicit)]
    public struct PROPVARIANT {
        [FieldOffset(0)] public ushort vt;
        [FieldOffset(8)] public IntPtr pointerValue;
    }

    [Guid("5a2f3d42-c004-4b2f-9c8f6f5f4c1bd234".Replace("d234","d234")), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPolicyConfig {
        int GetMixFormat();
        int GetDeviceFormat();
        int ResetDeviceFormat();
        int SetDeviceFormat();
        int GetProcessingPeriod();
        int SetProcessingPeriod();
        int GetShareMode();
        int SetShareMode();
        int GetPropertyValue();
        int SetPropertyValue();
        int SetDefaultEndpoint([MarshalAs(UnmanagedType.LPWStr)] string deviceId, int role);
    }

    public static string GetDeviceName(object deviceObj) {
        IPropertyStore store;
        device.OpenPropertyStore(0, out store);
        PROPERTYKEY key = new PROPERTYKEY { fmtid = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"), pid = 14 };
        PROPVARIANT value;
        store.GetValue(ref key, out value);
        string name = (value.vt == 31) ? Marshal.PtrToStringUni(value.pointerValue) : "?";
        return name;
    }

    public static void Run(bool setToSpeakers) {
        var enumerator = (IMMDeviceEnumerator)(object)new MMDeviceEnumeratorCom();
        IMMDeviceCollection devices;
        enumerator.EnumAudioEndpoints(0, 1, out devices);
        int count; devices.GetCount(out count);

        IMMDevice def; enumerator.GetDefaultAudioEndpoint(0, 0, out def);
        IntPtr defId; def.GetId(out defId);
        string defIdStr = Marshal.PtrToStringUni(defId);
        Console.WriteLine("DEFAULT: " + GetDeviceName(def));

        for (int i = 0; i < count; i++) {
            IMMDevice d; devices.Item(i, out d);
            IntPtr id; d.GetId(out id);
            string idStr = Marshal.PtrToStringUni(id);
            Console.WriteLine((idStr == defIdStr ? "* " : "  ") + GetDeviceName(d));
        }

        if (setToSpeakers) {
            for (int i = 0; i < count; i++) {
                IMMDevice d; devices.Item(i, out d);
                string name = GetDeviceName(d);
                if (!name.ToLower().Contains("realtek") || !name.ToLower().Contains("динамик")) continue;
                IntPtr id; d.GetId(out id);
                var policyConfig = (IPolicyConfig)(object)new PolicyConfigCom();
                string idStr2 = Marshal.PtrToStringUni(id);
                policyConfig.SetDefaultEndpoint(idStr2, 0);
                policyConfig.SetDefaultEndpoint(idStr2, 1);
                policyConfig.SetDefaultEndpoint(idStr2, 2);
                Console.WriteLine("SET DEFAULT -> " + name);
                break;
            }
        }
    }

    [ComImport, Guid("870af99c-171d-4f9e-af0d-e63df40c2bc9")]
    class PolicyConfigCom {}
}
'@
Add-Type -TypeDefinition $src
[AudioEndpoints]::Run($SetToSpeakers)
