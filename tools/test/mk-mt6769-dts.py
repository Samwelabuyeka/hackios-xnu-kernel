#!/usr/bin/env python3
"""Write a synthetic MT6769 device tree for testing tools/mtk-soc-args.py.

Not a copy of any phone's tree: the addresses and sizes are made up, and the structure is only as
complete as the tool under test needs. The point is that the tool's output can be checked against
values written here, so a change that reads the wrong cell or the wrong node fails the test
instead of silently producing a boot-arg a phone would then crash on.
"""
import subprocess
import sys

DTS = r"""
/dts-v1/;
/ {
	model = "Synthetic MT6769 (test only)";
	compatible = "mediatek,MT6769", "mediatek,mt6769";
	#address-cells = <2>;
	#size-cells = <2>;
	interrupt-parent = <&gic>;

	chosen {
		bootargs = "console=ttyS0,921600n8";
	};

	aliases {
		serial0 = "/soc/serial@11002000";
	};

	memory@40000000 {
		device_type = "memory";
		reg = <0x0 0x40000000 0x0 0x80000000>,   /* 2 GB at 1 GB */
		      <0x1 0x00000000 0x0 0x40000000>;   /* 1 GiB above 4 GB */
	};

	reserved-memory {
		#address-cells = <2>;
		#size-cells = <2>;
		ranges;

		atf-reserved-memory@43000000 {
			reg = <0x0 0x43000000 0x0 0x300000>;
			no-map;
		};
		tee-reserved-memory@54600000 {
			reg = <0x0 0x54600000 0x0 0x500000>;
			no-map;
		};
		mdss-reserved@9fe00000 {
			reg = <0x0 0x9f000000 0x0 0x1000000>;
			no-map;
		};
	};

	soc {
		compatible = "simple-bus";
		#address-cells = <2>;
		#size-cells = <2>;
		ranges;

		serial@11002000 {
			compatible = "mediatek,mt6577-uart";
			reg = <0x0 0x11002000 0x0 0x1000>;
			reg-shift = <2>;
			clock-frequency = <921600>;
			status = "okay";
		};

		topckgen@10000000 {
			compatible = "mediatek,mt6769-topckgen";
			reg = <0x0 0x10000000 0x0 0x1000>;
		};

		mmsys@14000000 {
			compatible = "mediatek,mmsys";
			reg = <0x0 0x14000000 0x0 0x1000>;
		};

		disp@14010000 {
			compatible = "mediatek,mt6779-disp";
			reg = <0x0 0x14010000 0x0 0x1000>;
		};

		ufs@112b0000 {
			compatible = "mediatek,ufs";
			reg = <0x0 0x112b0000 0x0 0x1000>;
		};

		mmc@11230000 {
			compatible = "mediatek,mtk-sd";
			reg = <0x0 0x11230000 0x0 0x1000>;
		};

		gic: interrupt-controller@c000000 {
			compatible = "arm,gic-v3";
			reg = <0x0 0x0c000000 0x0 0x10000>;
		};
	};
};
"""


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/mt6769-synthetic.dtb"
    dts = "/tmp/mt6769-synthetic.dts"
    open(dts, "w").write(DTS)
    subprocess.run(["dtc", "-I", "dts", "-O", "dtb", "-o", out, dts], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(out)


if __name__ == "__main__":
    main()
