function [r_out,c_out] = lut_approx(r,c)
p00 =       216.3;%  (216.3, 216.3)
p10 =    -0.06859;%  (-0.06873, -0.06844)
p01 =      0.3216;%  (0.3215, 0.3217)
p20 =  -0.0002345;%  (-0.0002351, -0.0002339)
p11 =   0.0002141;%  (0.0002137, 0.0002144)
p30 =  -3.039e-09;%  (-3.798e-09, -2.28e-09)
p21 =   7.419e-07;%  (7.414e-07, 7.424e-07)

       p00 =       217.1;%  (217.1, 217.1)
       p10 =    -0.07984;%  (-0.07999, -0.07969)
       p01 =      0.3187;%  (0.3186, 0.3188)
       p20 =   -0.000205;%  (-0.0002056, -0.0002044)
       p11 =   0.0002506;%  (0.0002502, 0.0002509)
       p02 =   2.249e-06;%  (1.914e-06, 2.585e-06)
       p30 =    6.28e-10;%  (-1.548e-10, 1.411e-09)
       p21 =   6.417e-07;%  (6.412e-07, 6.423e-07)
       p12 =   -7.18e-11;%  (-4.585e-10, 3.149e-10)
       p03 =  -2.331e-09;%  (-2.661e-09, -2.001e-09)
       
c_out = p00 + p10*r + p01*c + p20*r^2 + p11*r*c + p30*r^3 + p21*r^2*c;


%Taylor Expansion
%Centered on [0,0] gives poor performance on larger rows and columns
% c_out = 217.9075 - 0.0987925*r + 0.3167*c - 0.0000987925*r^2 + 0.0003167*c*r + 0.0000000987925*r^3 + 0.0000003167*c*r^2;
%Centered on [208,320] gives almost perfect results
% c_out = (3167*c)/7920 + (63*r)/15488 + (7*(r - 208)^2)/1362944 + (7*(r - 208)^3)/1079451648 + (3167*(c - 320)*(r - 208)^2)/4967930880 + (3167*(c - 320)*(r - 208))/6272640 + 16650019/87120;


% z = (3167*r)/10000 - (39517*c)/400000 + (3167*c*r)/10000000 + (3167*c^2*r)/10000000000 + (3167*c^3*r)/10000000000000 + (3167*c^4*r)/10000000000000000 - (39517*c^2)/400000000 - (39517*c^3)/400000000000 - (39517*c^4)/400000000000000 - (39517*c^5)/400000000000000000 + 87163/400;
% z = (3167*c)/7600 + (5103*r)/1155200 + (5103*(r - 240)^2)/877952000 + (5103*(r - 240)^3)/667243520000 + (5103*(r - 240)^4)/507105075200000 + (3167*(c - 320)*(r - 240)^2)/4389760000 + (3167*(c - 320)*(r - 240)^3)/3336217600000 + (3167*(c - 320)*(r - 240))/5776000 + 5361563/28880;

% z = (3167*c)/7600 + (5103*r)/1155200 + (5103*(r - 240)^2)/877952000 + (5103*(r - 240)^3)/667243520000 + (3167*(c - 320)*(r - 240)^2)/4389760000 + (3167*(c - 320)*(r - 240))/5776000 + 5361563/28880;
% c_out = (3167*c)/7600 + (5103*r)/1155200 + (5103*(r - 240)^2)/877952000 + (5103*(r - 240)^3)/667243520000 + (3167*(c - 320)*(r - 240)^2)/4389760000 + (3167*(c - 320)*(r - 240))/5776000 + 5361563/28880;


p00 =       196.8;%  (196.8, 196.8)
p10 =     0.01674;%  (0.01672, 0.01676)
p01 =  -9.705e-06;%  (-2.72e-05, 7.788e-06)
p20 =   5.779e-05;%  (5.774e-05, 5.783e-05)
p11 =   3.536e-09;%  (-2.538e-08, 3.245e-08)
p02 =    2.24e-08;%  (-1.852e-09, 4.664e-08)
r_out = p00 + p10*r + p01*c + p20*r^2 + p11*r*c + p02*c^2;

%Taylor Expansion
% r_out = 196.4449 + 0.0259*r;
% r_out = 196.4449 + 0.0259449*r + 2.594489999999998985913407523185e-5*r^2;





