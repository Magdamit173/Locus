$fn = 48;
L = 40;
W = 12;
PCB_T = 0.8;
PAD_T = 0.08;
BLUE = [0.035, 0.26, 0.55];
GOLD = [0.78, 0.64, 0.39];
SILVER = [0.72, 0.74, 0.73];
DARK = [0.055, 0.06, 0.07];
WHITE = [0.94, 0.94, 0.90];
via_points = [
[0.55,0.55],[1.15,0.55],[1.75,0.55],[0.55,1.15],[1.15,1.15],[1.75,1.15],[0.55,1.75],[1.15,1.75],[1.75,1.75],
[4.2,0.45],[4.2,1.05],[4.2,1.65],[10.6,0.45],[10.6,1.0],[13.2,0.55],[14.1,0.55],[15.6,0.45],
[20.0,0.5],[30.2,0.55],[31.0,0.55],[32.0,0.55],[33.2,0.55],[34.4,0.55],[35.5,0.55],
[38.1,0.55],[38.8,0.55],[39.45,0.55],[38.1,1.2],[38.8,1.2],[39.45,1.2],
[0.55,10.4],[1.15,10.4],[1.75,10.4],[0.55,11.05],[1.15,11.05],[1.75,11.05],
[4.2,10.9],[4.2,11.5],[10.5,11.45],[12.5,11.45],[14.2,11.45],[16.0,11.5],[17.5,11.5],
[31.0,11.5],[32.2,11.5],[33.5,11.5],[34.8,11.5],[36.0,11.5],[38.1,10.1],[38.8,10.1],[39.45,10.1],
[38.1,10.8],[38.8,10.8],[39.45,10.8],[38.1,11.45],[38.8,11.45],[39.45,11.45],
[5.0,6.0],[5.6,6.0],[10.0,6.0],[11.0,6.0],[12.0,6.0],[13.2,4.8],[13.9,4.8],[14.8,4.8],
[15.0,5.5],[15.0,6.0],[21.2,5.1],[21.8,5.1],[22.6,5.1],[23.4,5.1],[29.9,5.0],[30.6,5.0],[31.3,5.0],
[14.7,11.0],[15.3,11.0],[22.0,11.1],[22.7,11.1],[30.8,10.9],[31.5,10.9]
];
module rounded_board() {
    translate([0.16,0.16,0])
        linear_extrude(height=PCB_T)
            offset(r=0.16)
                square([L-0.32,W-0.32]);
}
module pcb() {
    color(BLUE)
        difference() {
            rounded_board();
            for (p = via_points)
                translate([p[0],p[1],-0.02]) cylinder(h=PCB_T+0.04,d=0.2);
            translate([8.0,2.9,-0.02]) cylinder(h=PCB_T+0.04,d=1.25);
            translate([8.35,9.15,-0.02]) cylinder(h=PCB_T+0.04,d=1.25);
        }
}
module dark_vias(zbase) {
    color([0.025,0.04,0.065])
        for (p = via_points)
            translate([p[0],p[1],zbase]) cylinder(h=0.012,d=0.17);
}
module oval_contact(cx,cy,zbase,down=false) {
    color(GOLD)
        translate([cx,cy,zbase])
            difference() {
                linear_extrude(height=PAD_T)
                    scale([2.65,1.55]) circle(r=1);
                translate([0,0,-0.01]) cylinder(h=PAD_T+0.02,d=1.25);
            }
    color(SILVER)
        translate([cx,cy,down ? -0.012 : PCB_T+0.006])
            cylinder(h=0.025,d=0.78);
}
module rect_contact(cx,cy,zbase,sx,sy) {
    color(GOLD) translate([cx-sx/2,cy-sy/2,zbase]) cube([sx,sy,PAD_T]);
}
module pad_outline(cx,cy,sx,sy,zbase) {
    color(WHITE)
        translate([cx-sx/2,cy-sy/2,zbase])
            difference() {
                cube([sx,sy,0.025]);
                translate([0.16,0.16,-0.005]) cube([sx-0.32,sy-0.32,0.035]);
            }
}
module top_label(txt,x,y,sz) {
    color(WHITE)
        translate([x,y,PCB_T+0.006])
            linear_extrude(height=0.022)
                text(txt,size=sz,font="Liberation Sans:style=Bold",halign="left",valign="baseline",spacing=1);
}
module bottom_label(txt,x,y_visual,sz) {
    color(WHITE)
        translate([x,W-y_visual,-0.006])
            rotate([180,0,0])
                linear_extrude(height=0.022)
                    text(txt,size=sz,font="Liberation Sans:style=Bold",halign="left",valign="baseline",spacing=1);
}
module smd_resistor(cx,cy,sx=0.8,sy=1.65,h=0.68) {
    color([0.72,0.72,0.68]) {
        translate([cx-sx/2,cy-sy/2,PCB_T+0.015]) cube([sx,0.28,0.12]);
        translate([cx-sx/2,cy+sy/2-0.28,PCB_T+0.015]) cube([sx,0.28,0.12]);
    }
    color([0.12,0.13,0.14])
        translate([cx-sx/2+0.04,cy-sy/2+0.22,PCB_T+0.075]) cube([sx-0.08,sy-0.44,h-0.06]);
    color([0.86,0.82,0.68])
        translate([cx-sx/2+0.1,cy-0.12,PCB_T+h-0.005]) cube([sx-0.2,0.24,0.015]);
}
module ic6(cx,cy) {
    color([0.68,0.69,0.68])
        for (yy = [-0.48,0,0.48]) {
            translate([cx-1.0,cy+yy-0.12,PCB_T+0.03]) cube([0.47,0.24,0.15]);
            translate([cx+0.53,cy+yy-0.12,PCB_T+0.03]) cube([0.47,0.24,0.15]);
        }
    color(DARK)
        translate([cx-0.72,cy-0.7,PCB_T+0.13]) cube([1.44,1.4,0.72]);
    color([0.16,0.17,0.18])
        translate([cx-0.25,cy+0.35,PCB_T+0.86]) cube([0.5,0.12,0.025]);
}
module mosfet(cx,cy) {
    color([0.71,0.72,0.70])
        translate([cx-2.9,cy-2.35,PCB_T+0.015]) cube([5.8,4.7,0.12]);
    color([0.70,0.71,0.69])
        for (yy = [-1.55,1.55]) {
            translate([cx-3.0,cy+yy-0.4,PCB_T+0.03]) cube([0.7,0.8,0.22]);
            translate([cx+2.3,cy+yy-0.4,PCB_T+0.03]) cube([0.7,0.8,0.22]);
        }
    color([0.035,0.04,0.045])
        translate([cx-2.65,cy-2.25,PCB_T+0.12]) cube([5.3,4.5,2.455]);
    color([0.12,0.13,0.14])
        translate([cx-2.35,cy-1.95,PCB_T+2.58]) cube([4.7,3.9,0.015]);
}
module front_side() {
    oval_contact(8.0,2.9,PCB_T);
    oval_contact(8.35,9.15,PCB_T);
    rect_contact(2.15,6.5,PCB_T,2.5,4.0);
    rect_contact(37.7,5.7,PCB_T,2.5,4.0);
    pad_outline(2.15,6.5,2.8,4.3,PCB_T+0.001);
    pad_outline(37.7,5.7,2.8,4.3,PCB_T+0.001);
    rect_contact(13.1,6.0,PCB_T,2.0,0.65);
    rect_contact(13.0,11.0,PCB_T,1.9,0.65);
    rect_contact(13.0,8.45,PCB_T,1.0,0.55);
    smd_resistor(19.15,2.65,0.86,1.65,0.72);
    smd_resistor(21.25,2.65,0.9,1.55,0.72);
    ic6(24.75,2.65);
    smd_resistor(27.35,2.65,0.86,1.65,0.72);
    mosfet(18.25,8.45);
    mosfet(27.25,8.45);
    top_label("P+",3.1,0.55,1.05);
    top_label("P-",3.0,10.15,1.05);
    top_label("B+",0.35,8.65,1.15);
    top_label("B-",36.4,1.45,1.15);
    top_label("HXYP-1S",10.65,0.55,0.92);
    top_label("4012A",11.6,1.75,0.92);
    top_label("R1",18.65,0.35,0.7);
    top_label("C1",20.65,0.35,0.7);
    top_label("U1",24.1,0.35,0.7);
    top_label("R2",26.8,0.35,0.7);
}
module back_side() {
    oval_contact(8.0,2.9,-PAD_T);
    oval_contact(8.35,9.15,-PAD_T);
    rect_contact(2.15,6.5,-PAD_T,2.5,4.0);
    rect_contact(37.7,5.7,-PAD_T,2.5,4.0);
    pad_outline(2.15,6.5,2.8,4.3,-0.025);
    pad_outline(37.7,5.7,2.8,4.3,-0.025);
    bottom_label("P-",10.15,0.55,1.1);
    bottom_label("P+",10.4,8.85,1.1);
    bottom_label("B+",0.35,8.45,1.15);
    bottom_label("B-",36.4,8.6,1.15);
    bottom_label("HXYP-1S-4012A",10.0,4.55,1.18);
}
module via_faces() {
    dark_vias(0.0);
    dark_vias(PCB_T-0.012);
}
union() {
    pcb();
    front_side();
    back_side();
    via_faces();
}
