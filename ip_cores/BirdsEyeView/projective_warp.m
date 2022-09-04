function [img_out,Err_r,Err_c] = projective_warp(img_in, h, s_out, offset,build_lut,approx)
s = size(img_in);
h_inv = inv(h)';
h_inv = h_inv/h_inv(9)
%This inverse matrix coefficients can be easily implemented via LUT.
%Upon further insvestigation of LUT, it seems r2 only depends on r and not 
%c. This is due to the trapezoidal nature of bird-eye view transform.
%Furthermore, the homogenous scaling (bottom row) only very weakly depends
%on r and can be ignored. Thus, theoretically
%r2 = 0.0272*r + 196.0680; (approx.)
%c2 = 217.9 -0.3167*r 0.31678c (approx.)
%But does not work!!!!
%Further, accuracy could be acheived by quadratic or cubic regression!


img_out = zeros([s_out,3]);


if(build_lut==1)
    %Build LUT
    lut_rc = zeros(s_out(1),s_out(2),2);
    for r=1:s_out(1)-1,
        for c=1:s_out(2)-1,
            i = h_inv*[c+offset(2);r+offset(1);1];
            i = i/i(3);
            r2 = i(2);
            c2 = i(1);
            if((r2>1)&(r2<s(1))&(c2>1)&(c2<s(2)))
%                 fr2 = floor(r2);
%                 fc2 = floor(c2);
    %             dr = r2 - fr2;
    %             dc = c2 - fc2;
    %             img_out(r,c,:) = img_in(fr2,fc2,:)*(1-dr)*(1-dc) + img_in(fr2+1,fc2,:)*(dr)*(1-dc) + img_in(fr2,fc2+1,:)*(1-dr)*(dc) + img_in(fr2+1,fc2+1,:)*(dr)*(dc); 
                lut_rc(r,c,1) = c2; 
                lut_rc(r,c,2) = r2; 
            end
        end
    end
    save('lut_rc.mat','lut_rc');
else
    load('lut_rc.mat');
end
% load('sf');

%LUT-based Xformation
Err_r = zeros(1,(s_out(1)-1)*(s_out(2)-1));
Err_c = zeros(1,(s_out(1)-1)*(s_out(2)-1));
k = 1;
for r=1:s_out(1)-1,
    for c=1:s_out(2)-1,
        if(approx==0)
            c2 = lut_rc(r,c,1);
            r2 = lut_rc(r,c,2);
        else
            [r2,c2] = lut_approx(r,c);
            Err_r(k) = r2 - lut_rc(r,c,2);
            Err_c(k) = c2 - lut_rc(r,c,1);
            k = k + 1;
        end
%         if((r2>0)&(c2>0))
        if((r2>1)&(r2<s(1))&(c2>1)&(c2<s(2)))
            fr2 = floor(r2);
            fc2 = floor(c2);
            dr = r2 - fr2;
            dc = c2 - fc2;
            img_out(r,c,:) = img_in(fr2,fc2,:)*(1-dr)*(1-dc) + img_in(fr2+1,fc2,:)*(dr)*(1-dc) + img_in(fr2,fc2+1,:)*(1-dr)*(dc) + img_in(fr2+1,fc2+1,:)*(dr)*(dc); 
        end
    end
end

img_out = uint8(img_out);

